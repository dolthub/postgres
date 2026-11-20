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
 * A table's key and row layout from the catalog (doltgres' layout.rs): the
 * key its primary key gives a storage, each column's key type, the order of
 * its tree, which a comparator gives when a column is compared through its
 * btree class, and a slot's row as a row's forming takes it.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "doltgres/access/key.h"
#include "doltgres/doltgres/entries.h"
#include "doltgres/doltgres/layout.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/blob.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/txn/entry.h"
#include "pg_calls.h"

#ifdef DOLTGRES_TEST_DOLTGRES
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "layout.rs"

/* The element layouts of layout's vectors. */
#define DOLTGRES_LAYOUT_BYTES 1, 1
#define DOLTGRES_LAYOUT_KEY_COLUMN sizeof(TxnBaseKeyColumn), _Alignof(TxnBaseKeyColumn)
#define DOLTGRES_LAYOUT_KEYED sizeof(access_TupleUsizeOrderKeyType_t), _Alignof(access_TupleUsizeOrderKeyType_t)
#define DOLTGRES_LAYOUT_ATTRIBUTE sizeof(AccessKeyAttribute), _Alignof(AccessKeyAttribute)
#define DOLTGRES_LAYOUT_LABEL sizeof(access_TupleU32F32_t), _Alignof(access_TupleU32F32_t)

/* doltgres_layout_fork returns a fork error of Postgres' flavor with a literal message. */
static PgPgError
doltgres_layout_fork(const char *sqlstate, const char *message)
{
	StdString text;

	std_string_from_Checked(&text, message, strlen(message));
	return pg_PgError_fork((const uint8 *) sqlstate, &text);
}

/* doltgres_layout_typed returns a key type of a type in byte order with an inner part of no data. */
static AccessKeyKeyType
doltgres_layout_typed(uint32 typid, AccessKeyInnerTag tag)
{
	AccessKeyKeyType key_type = {.typid = typid, .collation = {ACCESS_KEY_COLLATION_BYTES}, .inner = {.tag = tag}};

	return key_type;
}

/* doltgres_layout_whole returns the key type of a container any of whose parts is compared through its class. */
static AccessKeyKeyType
doltgres_layout_whole(uint32 typid)
{
	AccessKeyKeyType key_type = doltgres_layout_typed(typid, ACCESS_KEY_INNER_COMPARED);

	key_type.inner.u.by_value = false;
	return key_type;
}

/* doltgres_layout_is_compared reports whether a key type is compared through its class. */
static bool
doltgres_layout_is_compared(const AccessKeyKeyType *key_type)
{
	return key_type->inner.tag == ACCESS_KEY_INNER_COMPARED;
}

/* doltgres_layout_boxed_frame_t is a box's making's state across its pad: the value it takes, which an unwind drops. */
typedef struct doltgres_layout_boxed_frame_t
{
	AccessKeyKeyType *value;
	AccessKeyKeyType *result;
} doltgres_layout_boxed_frame_t;

/* doltgres_layout_boxed_body allocates the box and moves the value into it. */
static void
doltgres_layout_boxed_body(void *frame_)
{
	doltgres_layout_boxed_frame_t *frame = frame_;

	frame->result = std_box_new_uninit_Checked(sizeof(AccessKeyKeyType), _Alignof(AccessKeyKeyType));
	*frame->result = *frame->value;
}

/* doltgres_layout_boxed_pad drops the value of an unwound box. */
static void
doltgres_layout_boxed_pad(void *frame_)
{
	access_key_KeyType_drop(((doltgres_layout_boxed_frame_t *) frame_)->value);
}

/* doltgres_layout_boxed returns Box::new of a key type, which it takes. */
static AccessKeyKeyType *
doltgres_layout_boxed(AccessKeyKeyType *value)
{
	doltgres_layout_boxed_frame_t frame = {value, NULL};

	std_landing(&frame, doltgres_layout_boxed_body, doltgres_layout_boxed_pad);
	return frame.result;
}

/*
 * doltgres_layout_storage answers a type's stored length, whether it is
 * passed by value, and its alignment, which type_storage packs in one value.
 */
bool
doltgres_layout_storage(const PgMayRaise *raise, const PgCatalog *catalog, uint32 typid, int16 *result_0,
						bool *result_1, uint8 *result_2, PgPgError *error)
{
	uint32		packed;

	if (!pg_calls_type_storage(raise, catalog, typid, &packed, error))
		return false;
	*result_0 = (int16) (uint16) (packed & 0xffff);
	*result_1 = (packed >> 16 & 1) == 1;
	*result_2 = (uint8) (packed >> 24);
	return true;
}

/*
 * doltgres_layout_collation answers how a key type's text sorts under a
 * collation: by its bytes under none or one that sorts as C does, and by its
 * sort keys under any other.
 */
static bool
doltgres_layout_collation(const PgMayRaise *raise, const PgCatalog *catalog, uint32 collid,
						  AccessKeyCollation *result, PgPgError *error)
{
	int32		sort;

	*result = (AccessKeyCollation)
	{
		ACCESS_KEY_COLLATION_BYTES
	};
	if (collid == 0)
		return true;
	if (!pg_calls_collation_sort(raise, catalog, collid, &sort, error))
		return false;
	if ((sort & 1) != 0)
		return true;
	if ((sort & 4) != 0)
	{
		result->tag = ACCESS_KEY_COLLATION_SORTED;
		result->id = collid;
		result->deterministic = (sort & 2) != 0;
		return true;
	}
	{
		static const StdFmtPiece pieces[] = {STD_FMT_LIT("collation "), STD_FMT_ARG(0),
		STD_FMT_LIT(" has no sort keys")};
		StdFmtArg	args[] = {{&collid, std_u32_Display_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};
		StdString text;

		std_fmt_format_Checked(&text, &message);
		*error = pg_PgError_fork((const uint8 *) "XX000", &text);
	}
	return false;
}

/* doltgres_layout_enum_labels_frame_t is enum_labels' state across its pad: the labels' bytes, while live. */
typedef struct doltgres_layout_enum_labels_frame_t
{
	const PgMayRaise *raise;
	const PgCatalog *catalog;
	uint32		typid;
	StdVec	   *result;
	bool		live;
	PgPgError  *error;
	bool		ok;
} doltgres_layout_enum_labels_frame_t;

/* doltgres_layout_enum_labels_pad frees the labels' bytes. */
static void
doltgres_layout_enum_labels_pad(void *frame_)
{
	doltgres_layout_enum_labels_frame_t *frame = frame_;

	if (frame->live)
		std_vec_free_Checked(frame->result, DOLTGRES_LAYOUT_BYTES);
	frame->live = false;
}

/* doltgres_layout_enum_labels_body asks for the labels until the bytes hold them all. */
static void
doltgres_layout_enum_labels_body(void *frame_)
{
	doltgres_layout_enum_labels_frame_t *frame = frame_;
	uint8		zero = 0;

	std_vec_u8_from_elem(frame->result, 0, 64);
	frame->live = true;
	for (;;)
	{
		size_t		count;

		if (!pg_calls_enum_labels(frame->raise, frame->catalog, frame->typid, frame->result->ptr, frame->result->len,
								  &count, frame->error))
			return;
		if (STD_MUL(usize, 8, count, 221, 12) <= frame->result->len)
		{
			frame->result->len = STD_MUL(usize, 8, count, 222, 29);
			frame->live = false;
			frame->ok = true;
			return;
		}
		std_vec_resize(frame->result, STD_MUL(usize, 8, count, 225, 23), &zero, DOLTGRES_LAYOUT_BYTES);
	}
}

/*
 * doltgres_layout_enum_labels answers an enum's labels' OIDs and sort orders,
 * each OID's four bytes and then its order's.
 */
static bool
doltgres_layout_enum_labels(const PgMayRaise *raise, const PgCatalog *catalog, uint32 typid, StdVec *result,
							PgPgError *error)
{
	doltgres_layout_enum_labels_frame_t frame = {raise, catalog, typid, result, false, error, false};

	std_landing(&frame, doltgres_layout_enum_labels_body, doltgres_layout_enum_labels_pad);
	doltgres_layout_enum_labels_pad(&frame);
	return frame.ok;
}

/* doltgres_layout_label_cmp orders two enum labels by their OIDs, sort_by_key's key. */
static int
doltgres_layout_label_cmp(const void *a, const void *b)
{
	uint32		x = ((const access_TupleU32F32_t *) a)->f0;
	uint32		y = ((const access_TupleU32F32_t *) b)->f0;

	return x < y ? -1 : x > y ? 1 : 0;
}

/*
 * doltgres_layout_key_type_frame_t is key_type's state across its pad: a
 * part's key type, a row's attributes (a Vec<Attribute>), and an enum's
 * labels' bytes and labels, each while live.
 */
typedef struct doltgres_layout_key_type_frame_t
{
	const PgMayRaise *raise;
	const PgCatalog *catalog;
	uint32		typid;
	uint32		collid;
	AccessKeyKeyType part;
	bool		part_live;
	StdVec		attributes;
	bool		attributes_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		labels;
	bool		labels_live;
	AccessKeyKeyType *result;
	PgPgError  *error;
	bool		ok;
} doltgres_layout_key_type_frame_t;

/* doltgres_layout_Attribute_drop is Attribute's drop glue, an element of a Vec<Attribute>'s. */
static void
doltgres_layout_Attribute_drop(void *attribute)
{
	access_key_KeyType_drop(&((AccessKeyAttribute *) attribute)->key_type);
}

/* doltgres_layout_key_type_pad drops what key_type holds. */
static void
doltgres_layout_key_type_pad(void *frame_)
{
	doltgres_layout_key_type_frame_t *frame = frame_;

	if (frame->labels_live)
		std_vec_free_Checked(&frame->labels, DOLTGRES_LAYOUT_LABEL);
	frame->labels_live = false;
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, DOLTGRES_LAYOUT_BYTES);
	frame->bytes_live = false;
	if (frame->attributes_live)
	{
		std_ptr_drop_in_place_slice(frame->attributes.ptr, frame->attributes.len, sizeof(AccessKeyAttribute),
									doltgres_layout_Attribute_drop);
		std_vec_free_Checked(&frame->attributes, DOLTGRES_LAYOUT_ATTRIBUTE);
	}
	frame->attributes_live = false;
	if (frame->part_live)
		access_key_KeyType_drop(&frame->part);
	frame->part_live = false;
}

/* doltgres_layout_key_type_part reads a part's key type into the frame, live. */
static bool
doltgres_layout_key_type_part(doltgres_layout_key_type_frame_t *frame, uint32 typid, uint32 collid)
{
	if (!doltgres_layout_key_type(frame->raise, frame->catalog, typid, collid, &frame->part, frame->error))
		return false;
	frame->part_live = true;
	return true;
}

/* doltgres_layout_key_type_row reads a row type's attributes' key types, or answers it whole for one compared. */
static bool
doltgres_layout_key_type_row(doltgres_layout_key_type_frame_t *frame)
{
	int32		natts;

	if (!pg_calls_row_natts(frame->raise, frame->catalog, frame->typid, &natts, frame->error))
		return false;
	std_vec_new(&frame->attributes, DOLTGRES_LAYOUT_ATTRIBUTE);
	frame->attributes_live = true;
	for (int32 i = 0; i < natts; i++)
	{
		uint64		word;
		uint32		collation;
		PgAttOption att;
		AccessKeyAttribute attribute;

		if (!pg_calls_row_att(frame->raise, frame->catalog, frame->typid, i, &word, frame->error))
			return false;
		att = pg_Att_of(word);
		if (!att.has)
		{
			*frame->error = doltgres_layout_fork("XX000", "a row type lost an attribute");
			return false;
		}
		if (pg_Att_dropped(att.att))
		{
			frame->part = access_key_KeyType_of(pg_Att_type_id(att.att));
			frame->part_live = true;
		}
		else if (!pg_calls_row_att_collation(frame->raise, frame->catalog, frame->typid, i, &collation,
											 frame->error) ||
				 !doltgres_layout_key_type_part(frame, pg_Att_type_id(att.att), collation))
			return false;
		if (doltgres_layout_is_compared(&frame->part))
		{
			*frame->result = doltgres_layout_whole(frame->typid);
			return true;
		}
		attribute = (AccessKeyAttribute)
		{
			.key_type = frame->part, .len = pg_Att_length(att.att), .by_value = pg_Att_by_value(att.att),
				.align = pg_Att_align(att.att), .dropped = pg_Att_dropped(att.att)
		};
		std_vec_push_Checked(&frame->attributes, &attribute, DOLTGRES_LAYOUT_ATTRIBUTE);
		frame->part_live = false;
	}
	*frame->result = doltgres_layout_typed(frame->typid, ACCESS_KEY_INNER_ROW);
	frame->result->inner.u.row = frame->attributes;
	frame->attributes_live = false;
	return true;
}

/* doltgres_layout_key_type_enum reads an enum's labels' sort orders by their OIDs. */
static bool
doltgres_layout_key_type_enum(doltgres_layout_key_type_frame_t *frame)
{
	if (!doltgres_layout_enum_labels(frame->raise, frame->catalog, frame->typid, &frame->bytes, frame->error))
		return false;
	frame->bytes_live = true;
	std_vec_new(&frame->labels, DOLTGRES_LAYOUT_LABEL);
	frame->labels_live = true;
	for (size_t at = 0; at + 8 <= frame->bytes.len; at += 8)
	{
		const uint8 *label = (const uint8 *) frame->bytes.ptr + at;
		access_TupleU32F32_t pair;
		uint32		bits = (uint32) label[4] | (uint32) label[5] << 8 | (uint32) label[6] << 16 |
			(uint32) label[7] << 24;

		pair.f0 = (uint32) label[0] | (uint32) label[1] << 8 | (uint32) label[2] << 16 | (uint32) label[3] << 24;
		memcpy(&pair.f1, &bits, sizeof(pair.f1));
		std_vec_push_Checked(&frame->labels, &pair, DOLTGRES_LAYOUT_LABEL);
	}
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, DOLTGRES_LAYOUT_BYTES);
	qsort(frame->labels.ptr, frame->labels.len, sizeof(access_TupleU32F32_t), doltgres_layout_label_cmp);
	*frame->result = doltgres_layout_typed(frame->typid, ACCESS_KEY_INNER_ENUM);
	frame->result->inner.u.labels = frame->labels;
	frame->labels_live = false;
	return true;
}

/* doltgres_layout_key_type_body reads the type's form from the catalog and makes its key type. */
static void
doltgres_layout_key_type_body(void *frame_)
{
	doltgres_layout_key_type_frame_t *frame = frame_;
	const PgMayRaise *raise = frame->raise;
	const PgCatalog *catalog = frame->catalog;
	uint32		typid = frame->typid;
	uint32		found;
	uint32		other;
	bool		is_enum;
	int16		len;
	bool		by_value;
	uint8		align;

	if (access_key_has_own_key(typid))
	{
		*frame->result = doltgres_layout_typed(typid, ACCESS_KEY_INNER_NONE);
		frame->ok = doltgres_layout_collation(raise, catalog, typid == JSONBOID ? DEFAULT_COLLATION_OID :
											  frame->collid, &frame->result->collation, frame->error);
		return;
	}
	if (!pg_calls_type_element(raise, catalog, typid, &found, frame->error))
		return;
	if (found != 0)
	{
		if (!doltgres_layout_storage(raise, catalog, found, &len, &by_value, &align, frame->error) ||
			!doltgres_layout_key_type_part(frame, found, frame->collid))
			return;
		if (doltgres_layout_is_compared(&frame->part))
			*frame->result = doltgres_layout_whole(typid);
		else
		{
			frame->part_live = false;
			*frame->result = doltgres_layout_typed(typid, ACCESS_KEY_INNER_ARRAY);
			frame->result->inner.u.array = (AccessKeyStored)
			{
				doltgres_layout_boxed(&frame->part), len, by_value, align
			};
		}
		frame->ok = true;
		return;
	}
	if (!pg_calls_type_relid(raise, catalog, typid, &found, frame->error))
		return;
	if (found != 0)
	{
		frame->ok = doltgres_layout_key_type_row(frame);
		return;
	}
	if (!pg_calls_multirange_range(raise, catalog, typid, &found, frame->error))
		return;
	if (found != 0)
	{
		if (!doltgres_layout_key_type_part(frame, found, 0))
			return;
		if (frame->part.inner.tag == ACCESS_KEY_INNER_RANGE)
		{
			frame->part_live = false;
			*frame->result = doltgres_layout_typed(typid, ACCESS_KEY_INNER_MULTIRANGE);
			frame->result->inner.u.multirange = doltgres_layout_boxed(&frame->part);
		}
		else
			*frame->result = doltgres_layout_whole(typid);
		frame->ok = true;
		return;
	}
	if (!pg_calls_range_subtype(raise, catalog, typid, &found, frame->error))
		return;
	if (found != 0)
	{
		uint32		class;
		uint32		collation;

		if (!pg_calls_range_subopclass(raise, catalog, typid, &class, frame->error) ||
			!pg_calls_default_btree_opclass(raise, catalog, found, &other, frame->error))
			return;
		if (class != other)
		{
			*frame->result = doltgres_layout_whole(typid);
			frame->ok = true;
			return;
		}
		if (!doltgres_layout_storage(raise, catalog, found, &len, &by_value, &align, frame->error) ||
			!pg_calls_range_collation(raise, catalog, typid, &collation, frame->error) ||
			!doltgres_layout_key_type_part(frame, found, collation))
			return;
		if (doltgres_layout_is_compared(&frame->part))
			*frame->result = doltgres_layout_whole(typid);
		else
		{
			frame->part_live = false;
			*frame->result = doltgres_layout_typed(typid, ACCESS_KEY_INNER_RANGE);
			frame->result->inner.u.range = (AccessKeyStored)
			{
				doltgres_layout_boxed(&frame->part), len, by_value, align
			};
		}
		frame->ok = true;
		return;
	}
	if (!pg_calls_type_base(raise, catalog, typid, &found, frame->error))
		return;
	if (found != typid)
	{
		frame->ok = doltgres_layout_key_type(raise, catalog, found, frame->collid, frame->result, frame->error);
		return;
	}
	if (!pg_calls_type_is_enum(raise, catalog, typid, &is_enum, frame->error))
		return;
	if (is_enum)
	{
		frame->ok = doltgres_layout_key_type_enum(frame);
		return;
	}
	if (!pg_calls_default_btree_opclass(raise, catalog, typid, &found, frame->error))
		return;
	if (found == 0 && !pg_calls_refuse_no_btree_class(raise, catalog, typid, frame->error))
		return;
	if (!pg_calls_opclass_input_type(raise, catalog, found, &other, frame->error))
		return;
	if (other != typid && access_key_has_own_key(other))
	{
		frame->ok = doltgres_layout_key_type(raise, catalog, other, frame->collid, frame->result, frame->error);
		return;
	}
	if (!doltgres_layout_storage(raise, catalog, typid, &len, &by_value, &align, frame->error))
		return;
	*frame->result = doltgres_layout_typed(typid, ACCESS_KEY_INNER_COMPARED);
	frame->result->inner.u.by_value = by_value;
	frame->ok = true;
}

/*
 * doltgres_layout_key_type answers the key type of a type under a collation
 * from the catalog: an array's, a range's, a multirange's, a row type's, a
 * domain's, and an enum's from their parts, a type whose default btree class
 * is for another type with a key of its own as that type's, and any other
 * type with a default btree class compared through the class. A type with no
 * default btree class is refused with Postgres' error.
 */
bool
doltgres_layout_key_type(const PgMayRaise *raise, const PgCatalog *catalog, uint32 typid, uint32 collid,
						 AccessKeyKeyType *result, PgPgError *error)
{
	doltgres_layout_key_type_frame_t frame = {.raise = raise, .catalog = catalog, .typid = typid, .collid = collid,
	.result = result, .error = error};

	std_landing(&frame, doltgres_layout_key_type_body, doltgres_layout_key_type_pad);
	doltgres_layout_key_type_pad(&frame);
	return frame.ok;
}

/* The element layouts of a comparator's columns and of its columns' inputs. */
#define DOLTGRES_LAYOUT_COMPARED \
	sizeof(doltgres_TupleOrderKeyTypeOptionCompared_t), _Alignof(doltgres_TupleOrderKeyTypeOptionCompared_t)
#define DOLTGRES_LAYOUT_COLLATED sizeof(doltgres_TupleOrderKeyTypeU32_t), _Alignof(doltgres_TupleOrderKeyTypeU32_t)

/* doltgres_layout_compared_drop is a Compared column's drop glue. */
static void
doltgres_layout_compared_drop(void *compared)
{
	access_key_KeyType_drop(&((doltgres_TupleOrderKeyTypeOptionCompared_t *) compared)->f1);
}

/* doltgres_layout_collated_drop is (Order, KeyType, u32)'s drop glue. */
static void
doltgres_layout_collated_drop(void *collated)
{
	access_key_KeyType_drop(&((doltgres_TupleOrderKeyTypeU32_t *) collated)->f1);
}

/* doltgres_layout_vec_drop frees a vector, dropping its elements from a place on. */
static void
doltgres_layout_vec_drop(StdVec *v, size_t from, size_t size, size_t align, void (*drop) (void *))
{
	if (v->len > from)
		std_ptr_drop_in_place_slice((char *) v->ptr + from * size, v->len - from, size, drop);
	std_vec_free_Checked(v, size, align);
}

/* doltgres_layout_VecKeyed_drop is key_columns' answer's drop glue, a Vec<(usize, Order, KeyType)>'s. */
void
doltgres_layout_VecKeyed_drop(void *columns)
{
	doltgres_layout_vec_drop(columns, 0, DOLTGRES_LAYOUT_KEYED, access_TupleUsizeOrderKeyType_drop);
}

/*
 * doltgres_layout_list_frame_t is a list's making's state across its pad:
 * what it reads by, its input's elements from a place on, and its answer
 * so far, while live.
 */
typedef struct doltgres_layout_list_frame_t
{
	const PgMayRaise *raise;
	const PgCatalog *catalog;
	const void *by;
	StdVec		input;
	size_t		next;
	bool		input_live;
	StdVec	   *result;
	bool		result_live;
	PgPgError  *error;
	bool		ok;
} doltgres_layout_list_frame_t;

/* doltgres_layout_catalog_key_pad frees the key's columns made so far. */
static void
doltgres_layout_catalog_key_pad(void *frame_)
{
	doltgres_layout_list_frame_t *frame = frame_;

	if (frame->result_live)
		std_vec_free_Checked(frame->result, DOLTGRES_LAYOUT_KEY_COLUMN);
	frame->result_live = false;
}

/*
 * doltgres_layout_catalog_key_body reads the primary key's columns and asks
 * each column's key type whether it is compared.
 */
static void
doltgres_layout_catalog_key_body(void *frame_)
{
	doltgres_layout_list_frame_t *frame = frame_;
	DgKeyColumn columns[INDEX_MAX_KEYS] = {0};
	int32		count;
	uint32		n;

	if (!pg_calls_rel_key_columns(frame->raise, frame->catalog, frame->by, columns, &count, frame->error))
		return;
	n = count < 0 ? 0 - (uint32) count : (uint32) count;
	std_range_to_check(n, INDEX_MAX_KEYS, RUST_FILE, 38, 20);
	std_vec_new(frame->result, DOLTGRES_LAYOUT_KEY_COLUMN);
	frame->result_live = true;
	for (uint32 i = 0; i < n; i++)
	{
		AccessKeyKeyType key_type;
		TxnBaseKeyColumn column;
		uint16		attnum = (uint16) columns[i].attnum;

		if (columns[i].attnum < 0)
			attnum = 0 - attnum;
		if (!doltgres_layout_key_type(frame->raise, frame->catalog, columns[i].type, columns[i].collation, &key_type,
									  frame->error))
			return;
		column = (TxnBaseKeyColumn)
		{
			.attnum = attnum, .class = columns[i].class_, .collation = columns[i].collation,
				.typid = columns[i].type, .compared = doltgres_layout_is_compared(&key_type)
		};
		access_key_KeyType_drop(&key_type);
		std_vec_push_Checked(frame->result, &column, DOLTGRES_LAYOUT_KEY_COLUMN);
	}
	frame->result_live = false;
	frame->ok = true;
}

/*
 * doltgres_layout_catalog_key answers the key a relation's primary key gives
 * a storage made for it, its columns in key order, each with the index's
 * operator class and collation for it and whether its key type is compared
 * through that class, or none for a table without one.
 */
bool
doltgres_layout_catalog_key(const PgMayRaise *raise, const PgCatalog *catalog, const PgRel *rel, TxnBaseKey *result,
							PgPgError *error)
{
	StdVec		columns;
	doltgres_layout_list_frame_t frame = {.raise = raise, .catalog = catalog, .by = rel, .result = &columns,
	.error = error};

	std_landing(&frame, doltgres_layout_catalog_key_body, doltgres_layout_catalog_key_pad);
	doltgres_layout_catalog_key_pad(&frame);
	if (frame.ok)
		*result = txn_base_Key_of(&columns);
	return frame.ok;
}

/* doltgres_layout_key_columns_pad frees the key columns made so far. */
static void
doltgres_layout_key_columns_pad(void *frame_)
{
	doltgres_layout_list_frame_t *frame = frame_;

	if (frame->result_live)
		doltgres_layout_vec_drop(frame->result, 0, DOLTGRES_LAYOUT_KEYED, access_TupleUsizeOrderKeyType_drop);
	frame->result_live = false;
}

/* doltgres_layout_key_columns_body reads each recorded column's key type. */
static void
doltgres_layout_key_columns_body(void *frame_)
{
	doltgres_layout_list_frame_t *frame = frame_;
	const StdVec *columns = frame->by;

	std_vec_with_capacity_Checked(frame->result, columns->len, DOLTGRES_LAYOUT_KEYED);
	frame->result_live = true;
	for (size_t i = 0; i < columns->len; i++)
	{
		const TxnBaseKeyColumn *column = &STD_VEC_AT(columns, TxnBaseKeyColumn, i);
		access_TupleUsizeOrderKeyType_t keyed = {.f0 = STD_SUB(usize, column->attnum, 1, 69, 21),
		.f1 = ENCODING_KEY_ORDER_ASC};

		if (!doltgres_layout_key_type(frame->raise, frame->catalog, column->typid, column->collation, &keyed.f2,
									  frame->error))
			return;
		std_vec_push_Checked(frame->result, &keyed, DOLTGRES_LAYOUT_KEYED);
	}
	frame->result_live = false;
	frame->ok = true;
}

/*
 * doltgres_layout_key_columns answers the columns of a storage's recorded
 * key as a slot keeps them beside a row, each column's position, its order,
 * and its key type, or none for a keyless storage's: a primary key is always
 * ascending with nulls last. A key no backend recorded is an internal error.
 */
bool
doltgres_layout_key_columns(const PgMayRaise *raise, const PgCatalog *catalog, const TxnBaseKey *key, StdVec *result,
							PgPgError *error)
{
	doltgres_layout_list_frame_t frame = {.raise = raise, .catalog = catalog, .result = result, .error = error};

	if (key->tag == TXN_BASE_KEY_NONE)
	{
		std_vec_new(result, DOLTGRES_LAYOUT_KEYED);
		return true;
	}
	if (key->tag == TXN_BASE_KEY_UNKNOWN)
	{
		*error = doltgres_layout_fork("XX000", "a storage was read with no key recorded");
		return false;
	}
	frame.by = &key->u.columns;
	std_landing(&frame, doltgres_layout_key_columns_body, doltgres_layout_key_columns_pad);
	doltgres_layout_key_columns_pad(&frame);
	return frame.ok;
}

/* doltgres_layout_keyed_of answers the keyed layout of a key's columns, as a slot keeps it. */
static bool
doltgres_layout_keyed_of(const PgMayRaise *raise, const PgCatalog *catalog, const TxnBaseKey *key, StdVec *result,
						 PgPgError *error)
{
	StdVec		columns;

	if (!doltgres_layout_key_columns(raise, catalog, key, &columns, error))
		return false;
	access_slot_keyed(columns.ptr, columns.len, result);
	doltgres_layout_VecKeyed_drop(&columns);
	return true;
}

/*
 * doltgres_layout_same_key reports whether two keys encode a table's rows
 * alike: the same columns, classes, collations, and comparison, and the same
 * key types, whatever types they name.
 */
bool
doltgres_layout_same_key(const PgMayRaise *raise, const PgCatalog *catalog, const TxnBaseKey *a, const TxnBaseKey *b,
						 bool *result, PgPgError *error)
{
	const StdVec *columns = &a->u.columns;
	const StdVec *others = &b->u.columns;
	StdVec		keyed_a;
	StdVec		keyed_b;
	bool		alike;

	if (a->tag != TXN_BASE_KEY_COLUMNS || b->tag != TXN_BASE_KEY_COLUMNS)
	{
		*result = txn_base_Key_PartialEq_eq(a, b);
		return true;
	}
	alike = columns->len == others->len;
	for (size_t i = 0; alike && i < columns->len; i++)
	{
		const TxnBaseKeyColumn *column = &STD_VEC_AT(columns, TxnBaseKeyColumn, i);
		const TxnBaseKeyColumn *other = &STD_VEC_AT(others, TxnBaseKeyColumn, i);

		alike = column->attnum == other->attnum && column->class == other->class &&
			column->collation == other->collation && column->compared == other->compared;
	}
	*result = false;
	if (!alike)
		return true;
	if (!doltgres_layout_keyed_of(raise, catalog, a, &keyed_a, error))
		return false;
	if (!doltgres_layout_keyed_of(raise, catalog, b, &keyed_b, error))
	{
		std_vec_free_Checked(&keyed_a, DOLTGRES_LAYOUT_BYTES);
		return false;
	}
	*result = keyed_a.len == keyed_b.len && memcmp(keyed_a.ptr, keyed_b.ptr, keyed_a.len) == 0;
	std_vec_free_Checked(&keyed_b, DOLTGRES_LAYOUT_BYTES);
	std_vec_free_Checked(&keyed_a, DOLTGRES_LAYOUT_BYTES);
	return true;
}

/*
 * doltgres_layout_class_key_type answers the key type of a column of a type
 * under a collation that an operator class orders: the type's own under the
 * type's default btree class, and otherwise one compared through the class.
 */
bool
doltgres_layout_class_key_type(const PgMayRaise *raise, const PgCatalog *catalog, uint32 typid, uint32 collation,
							   bool default_class, AccessKeyKeyType *result, PgPgError *error)
{
	int16		len;
	bool		by_value;
	uint8		align;

	if (default_class)
		return doltgres_layout_key_type(raise, catalog, typid, collation, result, error);
	if (!doltgres_layout_storage(raise, catalog, typid, &len, &by_value, &align, error))
		return false;
	*result = doltgres_layout_typed(typid, ACCESS_KEY_INNER_COMPARED);
	result->inner.u.by_value = by_value;
	return true;
}

/* doltgres_layout_compared_columns_pad drops the input's columns not yet taken and the answer's made so far. */
static void
doltgres_layout_compared_columns_pad(void *frame_)
{
	doltgres_layout_list_frame_t *frame = frame_;

	if (frame->result_live)
		doltgres_layout_vec_drop(frame->result, 0, DOLTGRES_LAYOUT_COMPARED, doltgres_layout_compared_drop);
	frame->result_live = false;
	if (frame->input_live)
		doltgres_layout_vec_drop(&frame->input, frame->next, DOLTGRES_LAYOUT_COLLATED, doltgres_layout_collated_drop);
	frame->input_live = false;
}

/* doltgres_layout_compared_function_t is compared_columns' function and its captures. */
typedef struct doltgres_layout_compared_function_t
{
	bool		(*function) (const void *captures, const PgMayRaise *raise, const PgCatalog *catalog, size_t at,
							 uint32 *result, PgPgError *error);
	const void *captures;
} doltgres_layout_compared_function_t;

/*
 * doltgres_layout_compared_columns_body takes each column and, for one
 * compared through its class, how its function is called.
 */
static void
doltgres_layout_compared_columns_body(void *frame_)
{
	doltgres_layout_list_frame_t *frame = frame_;
	const doltgres_layout_compared_function_t *function = frame->by;

	std_vec_with_capacity_Checked(frame->result, frame->input.len, DOLTGRES_LAYOUT_COMPARED);
	frame->result_live = true;
	while (frame->next < frame->input.len)
	{
		size_t		at = frame->next;
		doltgres_TupleOrderKeyTypeU32_t column = STD_VEC_AT(&frame->input, doltgres_TupleOrderKeyTypeU32_t, at);
		doltgres_TupleOrderKeyTypeOptionCompared_t compared = {.f0 = column.f0, .f1 = column.f1};
		int16		len;
		bool		by_value;
		uint8		align;

		if (column.f1.inner.tag == ACCESS_KEY_INNER_COMPARED)
		{
			compared.has_f2 = true;
			compared.f2.collation = column.f2;
			compared.f2.by_value = column.f1.inner.u.by_value;
			if (!function->function(function->captures, frame->raise, frame->catalog, at, &compared.f2.function,
									frame->error) ||
				!doltgres_layout_storage(frame->raise, frame->catalog, column.f1.typid, &len, &by_value, &align,
										 frame->error))
				return;
			compared.f2.len = len;
		}
		frame->next++;
		std_vec_push_Checked(frame->result, &compared, DOLTGRES_LAYOUT_COMPARED);
	}
	frame->result_live = false;
	frame->ok = true;
}

/*
 * doltgres_layout_compared_columns answers a comparator's key columns from
 * each column's order, key type, and collation, which it takes: a column
 * compared through its class calls the class's comparison function, which
 * the function gives by the column's place, under the collation.
 */
bool
doltgres_layout_compared_columns(const PgMayRaise *raise, const PgCatalog *catalog, StdVec *columns,
								 bool (*function) (const void *captures, const PgMayRaise *raise,
												   const PgCatalog *catalog, size_t at, uint32 *result,
												   PgPgError *error),
								 const void *function_captures, StdVec *result, PgPgError *error)
{
	doltgres_layout_compared_function_t called = {function, function_captures};
	doltgres_layout_list_frame_t frame = {.raise = raise, .catalog = catalog, .by = &called, .input = *columns,
	.input_live = true, .result = result, .error = error};

	std_landing(&frame, doltgres_layout_compared_columns_body, doltgres_layout_compared_columns_pad);
	doltgres_layout_compared_columns_pad(&frame);
	return frame.ok;
}

/* The element layouts of the probes' map and of an encoding's asked texts. */
#define DOLTGRES_LAYOUT_PROBE sizeof(DoltgresLayoutProbe), _Alignof(DoltgresLayoutProbe)
#define DOLTGRES_LAYOUT_ASK sizeof(doltgres_layout_Ask_t), _Alignof(doltgres_layout_Ask_t)

/* doltgres_layout_VecProbe_drop frees the probes' map, each entry's bytes and then the map. */
void
doltgres_layout_VecProbe_drop(void *probes)
{
	StdVec	   *map = probes;

	for (size_t i = 0; i < map->len; i++)
		std_vec_free_Checked(&STD_VEC_AT(map, DoltgresLayoutProbe, i).bytes, DOLTGRES_LAYOUT_BYTES);
	std_vec_free_Checked(map, DOLTGRES_LAYOUT_PROBE);
}

/* Probes' IoMutex, host's lock at the probes' map. */
#define HOST_LOCK_SCOPE extern
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecProbe
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) std_vec_new((place), DOLTGRES_LAYOUT_PROBE)
#define HOST_LOCK_TAKEN_DROP(place) doltgres_layout_VecProbe_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_LAYOUT_PROBES_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecProbe
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) std_vec_new((place), DOLTGRES_LAYOUT_PROBE)
#include "doltgres/host/lock_template.h"

/* doltgres_layout_BlobNames_Names_name is BlobNames' Names::name: the root of the blob a stored value is written as. */
static void
doltgres_layout_BlobNames_Names_name(const void *self, const uint8 *stored, size_t stored_len, uint8 result[32])
{
	ChunksAddressAddress name = trees_blob_blob_name(stored, stored_len, TREES_CHUNKER_DEFAULT_TARGET);

	memcpy(result, name.f0, sizeof(name.f0));
}

/* DOLTGRES_LAYOUT_BLOB_NAMES_NAMES_OPS is BlobNames' Names: a long value's name, its blob's root. */
const AccessKeyNamesOps DOLTGRES_LAYOUT_BLOB_NAMES_NAMES_OPS = {doltgres_layout_BlobNames_Names_name};

/*
 * DoltgresLayoutTableComparator is TableComparator: the key's columns, an
 * Arc<Compared>, and a scan's own long values, an Arc<Probes> or NULL for
 * a tree only stored keys reach.
 */
typedef struct DoltgresLayoutTableComparator
{
	StdArcInner *columns;
	StdArcInner *probes;
} DoltgresLayoutTableComparator;

/* doltgres_layout_Compared_drop is Compared's drop glue, an Arc<Compared>'s value's. */
void
doltgres_layout_Compared_drop(void *compared)
{
	doltgres_layout_vec_drop(compared, 0, DOLTGRES_LAYOUT_COMPARED, doltgres_layout_compared_drop);
}

/* doltgres_layout_Probes_drop is Probes' drop glue, an Arc<Probes>'s value's. */
void
doltgres_layout_Probes_drop(void *probes)
{
	doltgres_layout_VecProbe_drop(&((HostLockIoMutexVecProbe *) probes)->value);
}

/* doltgres_layout_TableComparator_drop is TableComparator's drop glue: its columns' Arc, then its probes'. */
static void
doltgres_layout_TableComparator_drop(void *comparator)
{
	DoltgresLayoutTableComparator *self = comparator;

	std_arc_drop(self->columns, sizeof(StdVec), _Alignof(StdVec), doltgres_layout_Compared_drop);
	if (self->probes != NULL)
		std_arc_drop(self->probes, sizeof(HostLockIoMutexVecProbe), _Alignof(HostLockIoMutexVecProbe),
					 doltgres_layout_Probes_drop);
}

/* DOLTGRES_LAYOUT_TABLE_COMPARATOR_TYPE is TableComparator's Any identity, which equal downcasts by. */
static const StdTypeId DOLTGRES_LAYOUT_TABLE_COMPARATOR_TYPE = {"doltgres::layout::TableComparator",
	sizeof(DoltgresLayoutTableComparator), _Alignof(DoltgresLayoutTableComparator),
doltgres_layout_TableComparator_drop};

static bool doltgres_layout_TableComparator_IoComparator_compare(void *self, HostIoIo *io, const uint8 *a,
																 size_t a_len, const uint8 *b, size_t b_len,
																 StdOrdering *result, ChunksErrorError *error);
static bool doltgres_layout_TableComparator_Equal_equal(void *self, HostIoIo *io, const uint8 *a, size_t a_len,
														const uint8 *b, size_t b_len, bool *result,
														HostError *error);

/* DOLTGRES_LAYOUT_TABLE_COMPARATOR_IO_COMPARATOR_OPS is TableComparator's IoComparator. */
static const TreesIoComparatorOps DOLTGRES_LAYOUT_TABLE_COMPARATOR_IO_COMPARATOR_OPS = {
&DOLTGRES_LAYOUT_TABLE_COMPARATOR_TYPE, doltgres_layout_TableComparator_IoComparator_compare};

/* DOLTGRES_LAYOUT_TABLE_COMPARATOR_EQUAL_OPS is TableComparator's Equal. */
static const TxnInsertEqualOps DOLTGRES_LAYOUT_TABLE_COMPARATOR_EQUAL_OPS = {
doltgres_layout_TableComparator_Equal_equal};

/* doltgres_layout_order_of returns KeyOrder::CompareIo of a table comparator, which it takes. */
static void
doltgres_layout_order_of(DoltgresLayoutTableComparator *comparator, TreesKeyOrder *result)
{
	result->tag = TREES_KEY_ORDER_COMPARE_IO;
	result->u.compare_io.inner = std_arc_new_dropping(comparator, sizeof(*comparator),
													  _Alignof(DoltgresLayoutTableComparator),
													  doltgres_layout_TableComparator_drop);
	result->u.compare_io.ops = &DOLTGRES_LAYOUT_TABLE_COMPARATOR_IO_COMPARATOR_OPS;
}

/*
 * doltgres_layout_index_order returns the order of an index's tree whose key
 * columns include one compared through its class, reading a scan's own long
 * values from its probes (NULL for none); it takes both Arcs.
 */
void
doltgres_layout_index_order(StdArcInner *columns, StdArcInner *probes, TreesKeyOrder *result)
{
	DoltgresLayoutTableComparator comparator = {columns, probes};

	doltgres_layout_order_of(&comparator, result);
}

/*
 * doltgres_layout_equal answers the class equality of a table's key order
 * whose key has a column compared through its class, or false for a key
 * ordered by its bytes.
 */
bool
doltgres_layout_equal(const TreesKeyOrder *order, TxnInsertEqual *result)
{
	if (order->tag != TREES_KEY_ORDER_COMPARE_IO ||
		order->u.compare_io.ops->type != &DOLTGRES_LAYOUT_TABLE_COMPARATOR_TYPE)
		return false;
	result->ops = &DOLTGRES_LAYOUT_TABLE_COMPARATOR_EQUAL_OPS;
	result->self = STD_ARC_VALUE(order->u.compare_io.inner, _Alignof(DoltgresLayoutTableComparator));
	return true;
}

/*
 * doltgres_layout_key_order_closure_2 is key_order's function: the btree order
 * function of a recorded column's class.
 */
static bool
doltgres_layout_key_order_closure_2(const void *captures, const PgMayRaise *raise, const PgCatalog *catalog, size_t at,
									uint32 *result, PgPgError *error)
{
	const StdVec *recorded = captures;

	std_index_check(at, recorded->len, RUST_FILE, 247, 50);
	return pg_calls_btree_order_function(raise, catalog, STD_VEC_AT(recorded, TxnBaseKeyColumn, at).class, result,
										 error);
}

/*
 * doltgres_layout_key_order_pad drops the recorded columns not yet taken and
 * the collated columns made so far.
 */
static void
doltgres_layout_key_order_pad(void *frame_)
{
	doltgres_layout_list_frame_t *frame = frame_;

	if (frame->result_live)
		doltgres_layout_vec_drop(frame->result, 0, DOLTGRES_LAYOUT_COLLATED, doltgres_layout_collated_drop);
	frame->result_live = false;
	if (frame->input_live)
		doltgres_layout_vec_drop(&frame->input, frame->next, DOLTGRES_LAYOUT_KEYED, access_TupleUsizeOrderKeyType_drop);
	frame->input_live = false;
}

/* doltgres_layout_key_order_body takes each recorded column with its key column's collation. */
static void
doltgres_layout_key_order_body(void *frame_)
{
	doltgres_layout_list_frame_t *frame = frame_;
	const StdVec *recorded = frame->by;
	size_t		count = Min(frame->input.len, recorded->len);

	std_vec_with_capacity_Checked(frame->result, count, DOLTGRES_LAYOUT_COLLATED);
	frame->result_live = true;
	while (frame->next < count)
	{
		access_TupleUsizeOrderKeyType_t *column = &STD_VEC_AT(&frame->input, access_TupleUsizeOrderKeyType_t,
															  frame->next);
		doltgres_TupleOrderKeyTypeU32_t entry = {column->f1, column->f2,
		STD_VEC_AT(recorded, TxnBaseKeyColumn, frame->next).collation};

		frame->next++;
		std_vec_push_Checked(frame->result, &entry, DOLTGRES_LAYOUT_COLLATED);
	}
	frame->result_live = false;
}

/*
 * doltgres_layout_key_order answers the order of a table's tree under its
 * recorded key's columns, which it takes: by the keys' bytes when every
 * column's type has a key of its own, and otherwise by a TableComparator.
 */
bool
doltgres_layout_key_order(const PgMayRaise *raise, const PgCatalog *catalog, const TxnBaseKey *key, StdVec *columns,
						  TreesKeyOrder *result, PgPgError *error)
{
	bool		compared = false;
	StdVec		collated;
	StdVec		made;
	doltgres_layout_list_frame_t frame = {.by = &key->u.columns, .input_live = true, .result = &collated};
	DoltgresLayoutTableComparator comparator = {0};

	for (size_t i = 0; i < columns->len && !compared; i++)
		compared = doltgres_layout_is_compared(&STD_VEC_AT(columns, access_TupleUsizeOrderKeyType_t, i).f2);
	if (key->tag != TXN_BASE_KEY_COLUMNS || !compared)
	{
		doltgres_layout_vec_drop(columns, 0, DOLTGRES_LAYOUT_KEYED, access_TupleUsizeOrderKeyType_drop);
		result->tag = TREES_KEY_ORDER_BYTES;
		return true;
	}
	frame.input = *columns;
	std_landing(&frame, doltgres_layout_key_order_body, doltgres_layout_key_order_pad);
	doltgres_layout_key_order_pad(&frame);
	if (!doltgres_layout_compared_columns(raise, catalog, &collated, doltgres_layout_key_order_closure_2,
										  &key->u.columns, &made, error))
		return false;
	comparator.columns = std_arc_new_dropping(&made, sizeof(made), _Alignof(StdVec), doltgres_layout_Compared_drop);
	doltgres_layout_order_of(&comparator, result);
	return true;
}

/*
 * doltgres_layout_Ask_t is an Ask, (u32, Vec<u8>, Vec<u8>): a text an
 * encoding asks a sort key of, its collation, its bytes, and its key, empty
 * until host's door answers it.
 */
typedef struct doltgres_layout_Ask_t
{
	uint32		f0;
	StdVec		f1;
	StdVec		f2;
} doltgres_layout_Ask_t;

/* doltgres_layout_Ask_drop is an Ask's drop glue. */
static void
doltgres_layout_Ask_drop(void *ask)
{
	std_vec_free_Checked(&((doltgres_layout_Ask_t *) ask)->f1, DOLTGRES_LAYOUT_BYTES);
	std_vec_free_Checked(&((doltgres_layout_Ask_t *) ask)->f2, DOLTGRES_LAYOUT_BYTES);
}

/*
 * doltgres_layout_Asked_SortKeys_sort_key is Asked's SortKeys::sort_key: the
 * key the second pass was given for a collation's text, or an empty one,
 * the text recorded with no key, on the first.
 */
static bool
doltgres_layout_Asked_SortKeys_sort_key(const void *self, uint32 collation, const uint8 *text, size_t text_len,
										StdVec *result, PgPgError *error)
{
	StdVec	   *asked = (StdVec *) self;
	doltgres_layout_Ask_t ask = {.f0 = collation};

	for (size_t i = 0; i < asked->len; i++)
	{
		const doltgres_layout_Ask_t *found = &STD_VEC_AT(asked, doltgres_layout_Ask_t, i);

		if (found->f0 == collation && found->f1.len == text_len && memcmp(found->f1.ptr, text, text_len) == 0)
		{
			std_vec_clone(result, &found->f2, DOLTGRES_LAYOUT_BYTES);
			return true;
		}
	}
	std_vec_reserve_Checked(asked, 1, DOLTGRES_LAYOUT_ASK);
	std_slice_to_vec_Checked(&ask.f1, text, text_len, DOLTGRES_LAYOUT_BYTES);
	std_vec_new(&ask.f2, DOLTGRES_LAYOUT_BYTES);
	std_vec_push_Checked(asked, &ask, DOLTGRES_LAYOUT_ASK);
	std_vec_new(result, DOLTGRES_LAYOUT_BYTES);
	return true;
}

/* doltgres_layout_Asked_Default_default is Asked's derived Default: no texts asked. */
static void
doltgres_layout_Asked_Default_default(StdVec *result)
{
	std_vec_new(result, DOLTGRES_LAYOUT_ASK);
}

/* DOLTGRES_LAYOUT_ASKED_SORT_KEYS_OPS is Asked's SortKeys. */
static const AccessKeySortKeysOps DOLTGRES_LAYOUT_ASKED_SORT_KEYS_OPS = {doltgres_layout_Asked_SortKeys_sort_key};

/* doltgres_layout_corrupt_key returns the error of a key whose column kept out of line has no whole name. */
static PgPgError
doltgres_layout_corrupt_key(void)
{
	return doltgres_layout_fork("XX001", "a key's column kept out of line has no whole name");
}

/* doltgres_layout_stored makes a compared value's bytes as host's door takes them, taking the value. */
static void
doltgres_layout_stored(AccessKeyDecoded *value, StdVec *result)
{
	uint8		bytes[8];

	if (value->tag == ACCESS_KEY_DECODED_BYTES)
	{
		*result = value->u.bytes;
		return;
	}
	for (int i = 0; i < 8; i++)
		bytes[i] = (uint8) (value->u.word >> (8 * i));
	std_slice_to_vec_Checked(result, bytes, sizeof(bytes), DOLTGRES_LAYOUT_BYTES);
}

/* doltgres_layout_long_frame_t is long's state across its pad: the probes' guard, while live. */
typedef struct doltgres_layout_long_frame_t
{
	HostLockIoMutexVecProbe *probes;
	HostIoHeld *held;
	const uint8 *name;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	StdVec	   *result;
	bool		found;
} doltgres_layout_long_frame_t;

/* doltgres_layout_long_pad drops the probes' guard. */
static void
doltgres_layout_long_pad(void *frame_)
{
	doltgres_layout_long_frame_t *frame = frame_;

	if (frame->guard_live)
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	frame->guard_live = false;
}

/* doltgres_layout_probe_cmp orders a name before, at, or after a probe's, the map's order. */
static int
doltgres_layout_probe_cmp(const void *name, const void *probe)
{
	return memcmp(name, ((const DoltgresLayoutProbe *) probe)->name, 32);
}

/* doltgres_layout_long_body looks the name up in the probes and clones its bytes out under the lock. */
static void
doltgres_layout_long_body(void *frame_)
{
	doltgres_layout_long_frame_t *frame = frame_;
	StdVec	   *map;
	const DoltgresLayoutProbe *probe;

	frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) frame->probes, &DOLTGRES_LAYOUT_PROBES_LOCK_TYPE,
										  frame->held);
	frame->guard_live = true;
	map = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
	probe = bsearch(frame->name, map->ptr, map->len, sizeof(DoltgresLayoutProbe), doltgres_layout_probe_cmp);
	if (probe != NULL)
	{
		std_vec_clone(frame->result, &probe->bytes, DOLTGRES_LAYOUT_BYTES);
		frame->found = true;
	}
}

/*
 * doltgres_layout_TableComparator_long answers the bytes a key's part out of
 * line names, from the scan's probes or from its blob; the probe's are
 * cloned out of the probes' lock, since the comparison that reads them runs
 * user code.
 */
static bool
doltgres_layout_TableComparator_long(const DoltgresLayoutTableComparator *self, HostIoIo *io, const uint8 *name,
									 size_t name_len, StdVec *result, HostError *error)
{
	PgPgError	failed;

	if (name_len != 32)
	{
		failed = doltgres_layout_corrupt_key();
		*error = pghost_held(&failed);
		return false;
	}
	if (self->probes != NULL)
	{
		HostIoHeld *held;
		StdAny		raise;
		doltgres_layout_long_frame_t frame = {.name = name, .result = result};

		frame.probes = STD_ARC_VALUE(self->probes, _Alignof(HostLockIoMutexVecProbe));
		host_io_Io_Sealed_parts(io, &held, &raise);
		frame.held = held;
		std_landing(&frame, doltgres_layout_long_body, doltgres_layout_long_pad);
		doltgres_layout_long_pad(&frame);
		if (frame.found)
			return true;
	}
	if (!doltgres_entries_blob_bytes(io, name, result, &failed))
	{
		*error = pghost_held(&failed);
		return false;
	}
	return true;
}

/*
 * doltgres_layout_TableComparator_value answers a compared column's value's
 * stored bytes as host's door takes them from its part of a key: a value in
 * line decoded, or one out of line read by its name.
 */
static bool
doltgres_layout_TableComparator_value(const DoltgresLayoutTableComparator *self, HostIoIo *io, StdBytes part,
									  const AccessKeyKeyType *key_type, EncodingKeyOrder order, StdVec *result,
									  HostError *error)
{
	bool		has;
	AccessKeyDecoded decoded;
	StdBytes	rest;
	PgPgError	failed;

	if (part.len > 0 && part.ptr[0] == ENCODING_KEY_OUT_OF_LINE)
		return doltgres_layout_TableComparator_long(self, io, part.ptr + 1, part.len - 1, result, error);
	if (!access_key_decode(part.ptr, part.len, key_type, order, &has, &decoded, &rest, &failed))
	{
		*error = pghost_held(&failed);
		return false;
	}
	if (!has)
	{
		static const char text[] = "a compared column's value is NULL";
		StdString	message;

		std_string_from_Checked(&message, text, sizeof(text) - 1);
		*error = host_Error_fork((const uint8 *) "XX000", &message);
		return false;
	}
	doltgres_layout_stored(&decoded, result);
	return true;
}

/*
 * doltgres_layout_whole_frame_t is whole's state across its pad: the long
 * value's stored bytes, the first pass's scratch, the texts it asked, and
 * the encoding, each while live.
 */
typedef struct doltgres_layout_whole_frame_t
{
	const DoltgresLayoutTableComparator *self;
	HostIoIo   *io;
	StdBytes	part;
	const AccessKeyKeyType *key_type;
	EncodingKeyOrder order;
	StdVec		stored;
	bool		stored_live;
	StdVec		scratch;
	bool		scratch_live;
	StdVec		asked;
	bool		asked_live;
	StdVec	   *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} doltgres_layout_whole_frame_t;

/* doltgres_layout_whole_pad drops what whole holds. */
static void
doltgres_layout_whole_pad(void *frame_)
{
	doltgres_layout_whole_frame_t *frame = frame_;

	if (frame->result_live)
		std_vec_free_Checked(frame->result, DOLTGRES_LAYOUT_BYTES);
	frame->result_live = false;
	if (frame->asked_live)
		doltgres_layout_vec_drop(&frame->asked, 0, DOLTGRES_LAYOUT_ASK, doltgres_layout_Ask_drop);
	frame->asked_live = false;
	if (frame->scratch_live)
		std_vec_free_Checked(&frame->scratch, DOLTGRES_LAYOUT_BYTES);
	frame->scratch_live = false;
	if (frame->stored_live)
		std_vec_free_Checked(&frame->stored, DOLTGRES_LAYOUT_BYTES);
	frame->stored_live = false;
}

/* doltgres_layout_whole_body reads the long value, asks its texts' sort keys, and encodes it again with them. */
static void
doltgres_layout_whole_body(void *frame_)
{
	doltgres_layout_whole_frame_t *frame = frame_;
	AccessKeyValue value = {ACCESS_KEY_VALUE_BYTES};
	AccessKeySortKeys asking = {&DOLTGRES_LAYOUT_ASKED_SORT_KEYS_OPS, &frame->asked};
	PgPgError	failed;

	if (!doltgres_layout_TableComparator_long(frame->self, frame->io, frame->part.ptr + 1, frame->part.len - 1,
											  &frame->stored, frame->error))
		return;
	frame->stored_live = true;
	value.u.bytes = (StdBytes)
	{
		frame->stored.ptr, frame->stored.len
	};
	doltgres_layout_Asked_Default_default(&frame->asked);
	frame->asked_live = true;
	std_vec_new(&frame->scratch, DOLTGRES_LAYOUT_BYTES);
	frame->scratch_live = true;
	if (!access_key_encode(&frame->scratch, asking, frame->key_type, true, value, frame->order, &failed))
	{
		*frame->error = pghost_held(&failed);
		return;
	}
	frame->scratch_live = false;
	std_vec_free_Checked(&frame->scratch, DOLTGRES_LAYOUT_BYTES);
	for (size_t i = 0; i < frame->asked.len; i++)
	{
		doltgres_layout_Ask_t *ask = &STD_VEC_AT(&frame->asked, doltgres_layout_Ask_t, i);
		StdVec		key;

		if (!host_sort_key(frame->io, ask->f0, ask->f1.ptr, ask->f1.len, &key, frame->error))
			return;
		std_vec_free_Checked(&ask->f2, DOLTGRES_LAYOUT_BYTES);
		ask->f2 = key;
	}
	std_vec_new(frame->result, DOLTGRES_LAYOUT_BYTES);
	frame->result_live = true;
	if (!access_key_encode(frame->result, asking, frame->key_type, true, value, frame->order, &failed))
	{
		*frame->error = pghost_held(&failed);
		return;
	}
	frame->result_live = false;
	frame->ok = true;
}

/*
 * doltgres_layout_TableComparator_whole answers a byte-ordered column's
 * whole encoding from its part of a key: the part in line, or for one out of
 * line its original's stored bytes read by its name and encoded again, its
 * texts' sort keys asked of the collation through host's door before the
 * encoding that takes them.
 */
static bool
doltgres_layout_TableComparator_whole(const DoltgresLayoutTableComparator *self, HostIoIo *io, StdBytes part,
									  const AccessKeyKeyType *key_type, EncodingKeyOrder order, StdVec *result,
									  HostError *error)
{
	doltgres_layout_whole_frame_t frame = {.self = self, .io = io, .part = part, .key_type = key_type,
	.order = order, .result = result, .error = error};

	if (part.len == 0 || part.ptr[0] != ENCODING_KEY_OUT_OF_LINE)
	{
		std_slice_to_vec_Checked(result, part.ptr, part.len, DOLTGRES_LAYOUT_BYTES);
		return true;
	}
	std_landing(&frame, doltgres_layout_whole_body, doltgres_layout_whole_pad);
	doltgres_layout_whole_pad(&frame);
	return frame.ok;
}

/* doltgres_layout_rank returns a column's marker as the comparator ranks it: a value out of line as a value in line. */
static uint8
doltgres_layout_rank(uint8 marker)
{
	return marker == ENCODING_KEY_OUT_OF_LINE ? ENCODING_KEY_VALUE : marker;
}

/* doltgres_layout_bytes_cmp is [u8]'s Ord. */
static StdOrdering
doltgres_layout_bytes_cmp(StdBytes a, StdBytes b)
{
	return std_bytes_Ord_cmp(a.ptr, a.len, b.ptr, b.len);
}

/*
 * doltgres_layout_column_frame_t is a column's comparison's state across its
 * pad: the two parts' values or whole encodings, each while live.
 */
typedef struct doltgres_layout_column_frame_t
{
	const DoltgresLayoutTableComparator *self;
	HostIoIo   *io;
	const doltgres_TupleOrderKeyTypeOptionCompared_t *column;
	StdBytes	px;
	StdBytes	py;
	StdVec		vx;
	bool		vx_live;
	StdVec		vy;
	bool		vy_live;
	StdOrdering *result;
	HostError  *error;
	bool		ok;
} doltgres_layout_column_frame_t;

/* doltgres_layout_column_pad drops the parts' values. */
static void
doltgres_layout_column_pad(void *frame_)
{
	doltgres_layout_column_frame_t *frame = frame_;

	if (frame->vy_live)
		std_vec_free_Checked(&frame->vy, DOLTGRES_LAYOUT_BYTES);
	frame->vy_live = false;
	if (frame->vx_live)
		std_vec_free_Checked(&frame->vx, DOLTGRES_LAYOUT_BYTES);
	frame->vx_live = false;
}

/*
 * doltgres_layout_column_body compares a column's two parts: through its
 * class's function for a compared column, by whole encodings for a part
 * kept out of line, and otherwise by the parts' bytes.
 */
static void
doltgres_layout_column_body(void *frame_)
{
	doltgres_layout_column_frame_t *frame = frame_;
	const doltgres_TupleOrderKeyTypeOptionCompared_t *column = frame->column;
	bool		value = frame->px.ptr[0] == ENCODING_KEY_VALUE || frame->px.ptr[0] == ENCODING_KEY_OUT_OF_LINE;

	if (column->has_f2 && value)
	{
		int32		compared;

		if (!doltgres_layout_TableComparator_value(frame->self, frame->io, frame->px, &column->f1, column->f0,
												   &frame->vx, frame->error))
			return;
		frame->vx_live = true;
		if (!doltgres_layout_TableComparator_value(frame->self, frame->io, frame->py, &column->f1, column->f0,
												   &frame->vy, frame->error))
			return;
		frame->vy_live = true;
		if (!host_btree_compare(frame->io, &column->f2, frame->vx.ptr, frame->vx.len, frame->vy.ptr, frame->vy.len,
								&compared, frame->error))
			return;
		if (column->f0.descending)
			*frame->result = 0 < compared ? STD_ORDERING_LESS : 0 > compared ? STD_ORDERING_GREATER :
				STD_ORDERING_EQUAL;
		else
			*frame->result = compared < 0 ? STD_ORDERING_LESS : compared > 0 ? STD_ORDERING_GREATER :
				STD_ORDERING_EQUAL;
	}
	else if (value &&
			 (frame->px.ptr[0] == ENCODING_KEY_OUT_OF_LINE || frame->py.ptr[0] == ENCODING_KEY_OUT_OF_LINE))
	{
		if (!doltgres_layout_TableComparator_whole(frame->self, frame->io, frame->px, &column->f1, column->f0,
												   &frame->vx, frame->error))
			return;
		frame->vx_live = true;
		if (!doltgres_layout_TableComparator_whole(frame->self, frame->io, frame->py, &column->f1, column->f0,
												   &frame->vy, frame->error))
			return;
		frame->vy_live = true;
		*frame->result = std_bytes_Ord_cmp(frame->vx.ptr, frame->vx.len, frame->vy.ptr, frame->vy.len);
	}
	else
		*frame->result = doltgres_layout_bytes_cmp(frame->px, frame->py);
	frame->ok = true;
}

/*
 * doltgres_layout_TableComparator_classes answers how two keys order by
 * their columns' classes alone, with no tie broken by their bytes, and the
 * bytes of each after its columns.
 */
static bool
doltgres_layout_TableComparator_classes(const DoltgresLayoutTableComparator *self, HostIoIo *io, StdBytes a,
										StdBytes b, StdOrdering *result, StdBytes *rest_a, StdBytes *rest_b,
										HostError *error)
{
	const StdVec *columns = STD_ARC_VALUE(self->columns, _Alignof(StdVec));
	StdBytes	x = a;
	StdBytes	y = b;

	*result = STD_ORDERING_EQUAL;
	for (size_t i = 0; i < columns->len; i++)
	{
		const doltgres_TupleOrderKeyTypeOptionCompared_t *column =
			&STD_VEC_AT(columns, doltgres_TupleOrderKeyTypeOptionCompared_t, i);
		uint8		mx;
		uint8		my;
		StdBytes	rx;
		StdBytes	ry;
		bool		bounded;
		PgPgError	failed;
		doltgres_layout_column_frame_t frame = {.self = self, .io = io, .column = column, .result = result,
		.error = error};

		if (x.len == 0 && y.len == 0)
			break;
		if (x.len == 0 || y.len == 0)
		{
			*result = x.len == 0 ? STD_ORDERING_LESS : STD_ORDERING_GREATER;
			break;
		}
		mx = doltgres_layout_rank(x.ptr[0]);
		my = doltgres_layout_rank(y.ptr[0]);
		if (mx != my)
		{
			*result = mx < my ? STD_ORDERING_LESS : STD_ORDERING_GREATER;
			break;
		}
		if (mx == ENCODING_KEY_PAST || mx == ENCODING_KEY_BELOW)
		{
			x = (StdBytes)
			{
				x.ptr + 1, x.len - 1
			};
			y = (StdBytes)
			{
				y.ptr + 1, y.len - 1
			};
			continue;
		}
		if (!access_key_skip_within(x.ptr, x.len, &column->f1, column->f0, x.len, &rx, &bounded, &failed) ||
			!access_key_skip_within(y.ptr, y.len, &column->f1, column->f0, y.len, &ry, &bounded, &failed))
		{
			*error = pghost_held(&failed);
			return false;
		}
		frame.px = (StdBytes)
		{
			x.ptr, x.len - rx.len
		};
		frame.py = (StdBytes)
		{
			y.ptr, y.len - ry.len
		};
		std_landing(&frame, doltgres_layout_column_body, doltgres_layout_column_pad);
		doltgres_layout_column_pad(&frame);
		if (!frame.ok)
			return false;
		if (*result != STD_ORDERING_EQUAL)
			break;
		x = rx;
		y = ry;
	}
	*rest_a = x;
	*rest_b = y;
	return true;
}

/* doltgres_layout_one reports whether a key's rest is exactly one byte, a marker. */
static bool
doltgres_layout_one(StdBytes rest, uint8 marker)
{
	return rest.len == 1 && rest.ptr[0] == marker;
}

/*
 * doltgres_layout_TableComparator_compare_keys answers how two keys of the
 * tree order: an index's record by its bytes, after every entry; otherwise
 * by their columns' classes, a key that ends first before a longer one, a
 * bound's BELOW and PAST before and after every key its prefix starts, and
 * every tie by the whole keys' bytes.
 */
static bool
doltgres_layout_TableComparator_compare_keys(const DoltgresLayoutTableComparator *self, HostIoIo *io, StdBytes a,
											 StdBytes b, StdOrdering *result, HostError *error)
{
	StdOrdering ordering;
	StdBytes	x;
	StdBytes	y;

	if ((a.len > 0 && a.ptr[0] == TXN_ENTRY_RECORD) || (b.len > 0 && b.ptr[0] == TXN_ENTRY_RECORD))
	{
		*result = doltgres_layout_bytes_cmp(a, b);
		return true;
	}
	if (!doltgres_layout_TableComparator_classes(self, io, a, b, &ordering, &x, &y, error))
		return false;
	if (ordering != STD_ORDERING_EQUAL || doltgres_layout_bytes_cmp(x, y) == STD_ORDERING_EQUAL)
		*result = ordering;
	else if (doltgres_layout_one(x, ENCODING_KEY_PAST) || doltgres_layout_one(y, ENCODING_KEY_BELOW))
		*result = STD_ORDERING_GREATER;
	else if (doltgres_layout_one(y, ENCODING_KEY_PAST) || doltgres_layout_one(x, ENCODING_KEY_BELOW))
		*result = STD_ORDERING_LESS;
	else
		*result = doltgres_layout_bytes_cmp(x, y);
	if (*result == STD_ORDERING_EQUAL)
		*result = doltgres_layout_bytes_cmp(a, b);
	return true;
}

/*
 * doltgres_layout_TableComparator_Equal_equal is TableComparator's
 * Equal::equal: whether two keys are equal by their columns' classes and
 * have the same bytes after their columns.
 */
static bool
doltgres_layout_TableComparator_Equal_equal(void *self, HostIoIo *io, const uint8 *a, size_t a_len, const uint8 *b,
											size_t b_len, bool *result, HostError *error)
{
	StdBytes	left = {a, a_len};
	StdBytes	right = {b, b_len};
	StdOrdering ordering;
	StdBytes	x;
	StdBytes	y;

	if (!doltgres_layout_TableComparator_classes(self, io, left, right, &ordering, &x, &y, error))
		return false;
	*result = ordering == STD_ORDERING_EQUAL && doltgres_layout_bytes_cmp(x, y) == STD_ORDERING_EQUAL;
	return true;
}

/* doltgres_layout_TableComparator_IoComparator_compare is TableComparator's IoComparator::compare. */
static bool
doltgres_layout_TableComparator_IoComparator_compare(void *self, HostIoIo *io, const uint8 *a, size_t a_len,
													 const uint8 *b, size_t b_len, StdOrdering *result,
													 ChunksErrorError *error)
{
	StdBytes	left = {a, a_len};
	StdBytes	right = {b, b_len};
	HostError	failed;

	if (doltgres_layout_TableComparator_compare_keys(self, io, left, right, result, &failed))
		return true;
	error->tag = CHUNKS_ERROR_ERROR_HOST;
	error->u.host = failed;
	return false;
}

/* doltgres_layout_Held_drop is Held's drop glue, an element of a Vec<Held>'s. */
static void
doltgres_layout_Held_drop(void *held)
{
	DoltgresLayoutHeld *self = held;

	if (self->has)
		std_vec_free_Checked(&self->f1, DOLTGRES_LAYOUT_BYTES);
}

/* doltgres_layout_VecHeld_drop is slot_row's held values' drop glue, a Vec<Held>'s. */
void
doltgres_layout_VecHeld_drop(void *held)
{
	doltgres_layout_vec_drop(held, 0, sizeof(DoltgresLayoutHeld), _Alignof(DoltgresLayoutHeld), doltgres_layout_Held_drop);
}

/*
 * doltgres_layout_slot_row_frame_t is slot_row's state across its pad: the
 * columns and values read so far, while live.
 */
typedef struct doltgres_layout_slot_row_frame_t
{
	const PgMayRaise *raise;
	const PgCatalog *catalog;
	const PgRel *rel;
	const PgSlot *source;
	StdVec	   *columns;
	StdVec	   *held;
	bool		live;
	PgPgError  *error;
	bool		ok;
} doltgres_layout_slot_row_frame_t;

/* doltgres_layout_slot_row_pad frees the columns and values read so far. */
static void
doltgres_layout_slot_row_pad(void *frame_)
{
	doltgres_layout_slot_row_frame_t *frame = frame_;

	if (frame->live)
	{
		doltgres_layout_VecHeld_drop(frame->held);
		std_vec_free_Checked(frame->columns, sizeof(AccessHeaptupleColumn), _Alignof(AccessHeaptupleColumn));
	}
	frame->live = false;
}

/* doltgres_layout_slot_row_body reads each attribute's column and the slot's value of it. */
static void
doltgres_layout_slot_row_body(void *frame_)
{
	doltgres_layout_slot_row_frame_t *frame = frame_;
	int32		natts = pg_calls_rel_natts(frame->rel);

	std_vec_new(frame->columns, sizeof(AccessHeaptupleColumn), _Alignof(AccessHeaptupleColumn));
	std_vec_with_capacity_Checked(frame->held, (size_t) natts, sizeof(DoltgresLayoutHeld),
								  _Alignof(DoltgresLayoutHeld));
	frame->live = true;
	for (int32 i = 0; i < natts; i++)
	{
		PgAttOption att = pg_Att_of(pg_calls_slot_att(frame->source, i));
		AccessHeaptupleColumn column = {0};
		DoltgresLayoutHeld held = {0};

		if (!att.has)
		{
			*frame->error = doltgres_layout_fork("XX000", "a slot's descriptor lost an attribute");
			return;
		}
		column.dropped = pg_calls_rel_att_dropped(frame->rel, i + 1);
		column.typid = pg_Att_type_id(att.att);
		column.len = pg_Att_length(att.att);
		column.by_value = pg_Att_by_value(att.att);
		column.align = pg_Att_align(att.att);
		if (!access_heaptuple_Storage_of(pg_calls_rel_att_storage(frame->rel, i + 1), &column.storage, frame->error))
			return;
		std_vec_push_Checked(frame->columns, &column, sizeof(column), _Alignof(AccessHeaptupleColumn));
		if (!column.dropped && !pg_calls_slot_value_null(frame->source, i))
		{
			held.has = true;
			if (column.by_value)
			{
				held.f0 = pg_calls_slot_value_word(frame->source, i);
				std_vec_new(&held.f1, DOLTGRES_LAYOUT_BYTES);
			}
			else
			{
				const uint8 *bytes;
				size_t		bytes_len;

				if (!pg_calls_slot_value_bytes(frame->raise, frame->catalog, frame->source, i, &bytes, &bytes_len,
											   frame->error))
					return;
				std_slice_to_vec_Checked(&held.f1, bytes, bytes_len, DOLTGRES_LAYOUT_BYTES);
			}
		}
		std_vec_push_Checked(frame->held, &held, sizeof(held), _Alignof(DoltgresLayoutHeld));
	}
	frame->live = false;
	frame->ok = true;
}

/*
 * doltgres_layout_slot_row answers the columns of a row a slot holds for a
 * relation, by the slot's descriptor, each stored and dropped as the
 * relation's is, and its values, which the slot holds deformed.
 */
bool
doltgres_layout_slot_row(const PgMayRaise *raise, const PgCatalog *catalog, const PgRel *rel, const PgSlot *source,
						 StdVec *result_0, StdVec *result_1, PgPgError *error)
{
	doltgres_layout_slot_row_frame_t frame = {raise, catalog, rel, source, result_0, result_1, false, error, false};

	std_landing(&frame, doltgres_layout_slot_row_body, doltgres_layout_slot_row_pad);
	doltgres_layout_slot_row_pad(&frame);
	return frame.ok;
}

/* doltgres_layout_values makes a row's values as forming takes them, borrowed from what slot_row read. */
void
doltgres_layout_values(const AccessHeaptupleColumn *columns, size_t columns_len, const DoltgresLayoutHeld *held,
					   size_t held_len, StdVec *result)
{
	size_t		len = Min(columns_len, held_len);

	std_vec_with_capacity_Checked(result, len, sizeof(AccessHeaptupleSet), _Alignof(AccessHeaptupleSet));
	for (size_t i = 0; i < len; i++)
	{
		AccessHeaptupleSet set = {ACCESS_HEAPTUPLE_SET_NULL};
		StdBytes	bytes = {held[i].f1.ptr, held[i].f1.len};

		if (held[i].has && columns[i].by_value)
		{
			set.tag = ACCESS_HEAPTUPLE_SET_WORD;
			set.u.word = held[i].f0;
		}
		else if (held[i].has && columns[i].len == -1)
		{
			set.tag = ACCESS_HEAPTUPLE_SET_VARLENA;
			set.u.varlena = bytes;
		}
		else if (held[i].has)
		{
			set.tag = ACCESS_HEAPTUPLE_SET_FIXED;
			set.u.fixed = bytes;
		}
		std_vec_push_Checked(result, &set, sizeof(set), _Alignof(AccessHeaptupleSet));
	}
}

#ifdef DOLTGRES_TEST_DOLTGRES

/* DOLTGRES_LAYOUT_TESTS_UNWRAP is Result::unwrap's panic text. */
#define DOLTGRES_LAYOUT_TESTS_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* doltgres_layout_tests_NoText_SortKeys_sort_key is NoText's SortKeys: no key of the cases asks for a text's sort key. */
static bool
doltgres_layout_tests_NoText_SortKeys_sort_key(const void *self, uint32 collation, const uint8 *text, size_t text_len,
											   StdVec *result, PgPgError *error)
{
	*error = doltgres_layout_fork("XX000", "the comparator's cases sort no text");
	return false;
}

/* DOLTGRES_LAYOUT_TESTS_NO_TEXT_SORT_KEYS_OPS is NoText's SortKeys, the cases' sorter. */
static const AccessKeySortKeysOps DOLTGRES_LAYOUT_TESTS_NO_TEXT_SORT_KEYS_OPS = {
doltgres_layout_tests_NoText_SortKeys_sort_key};

/* doltgres_layout_tests_compared returns a compared column's key type, the simulation's comparison function, and its order. */
static doltgres_TupleOrderKeyTypeOptionCompared_t
doltgres_layout_tests_compared(uint32 function)
{
	doltgres_TupleOrderKeyTypeOptionCompared_t column = {ENCODING_KEY_ORDER_ASC,
	doltgres_layout_typed(0, ACCESS_KEY_INNER_COMPARED), true, {function, 0, -1, false}};

	return column;
}

/* doltgres_layout_tests_comparator returns a comparator of compared columns of the simulation's functions, with no probes. */
static DoltgresLayoutTableComparator
doltgres_layout_tests_comparator(const uint32 *functions, size_t len)
{
	DoltgresLayoutTableComparator comparator = {0};
	StdVec		columns;

	std_vec_with_capacity_Checked(&columns, len, DOLTGRES_LAYOUT_COMPARED);
	for (size_t i = 0; i < len; i++)
	{
		doltgres_TupleOrderKeyTypeOptionCompared_t column = doltgres_layout_tests_compared(functions[i]);

		std_vec_push_Checked(&columns, &column, DOLTGRES_LAYOUT_COMPARED);
	}
	comparator.columns = std_arc_new(&columns, sizeof(StdVec), _Alignof(StdVec));
	return comparator;
}

/*
 * doltgres_layout_tests_key returns a key of columns of a comparator's key
 * types, each value its stored bytes, or a NULL pointer for a NULL.
 */
static StdVec
doltgres_layout_tests_key(const DoltgresLayoutTableComparator *comparator, const StdBytes *values, size_t len)
{
	const StdVec *columns = STD_ARC_VALUE(comparator->columns, _Alignof(StdVec));
	AccessKeySortKeys sorter = {&DOLTGRES_LAYOUT_TESTS_NO_TEXT_SORT_KEYS_OPS, NULL};
	StdVec		out;

	std_vec_new(&out, DOLTGRES_LAYOUT_BYTES);
	for (size_t i = 0; i < columns->len && i < len; i++)
	{
		const doltgres_TupleOrderKeyTypeOptionCompared_t *column =
			&STD_VEC_AT(columns, doltgres_TupleOrderKeyTypeOptionCompared_t, i);
		AccessKeyValue value = {ACCESS_KEY_VALUE_BYTES, .u.bytes = values[i]};
		PgPgError	error;
		bool		ok = access_key_encode(&out, sorter, &column->f1, values[i].ptr != NULL, value, column->f0,
										   &error);

		if (!ok)
			pg_PgError_drop(&error);
		STD_ASSERT(588, 13, ok,
				   "access::key::encode(&mut out, &NoText, key_type, value.map(Value::Bytes), *order).is_ok()");
	}
	return out;
}

/* doltgres_layout_tests_compare_keys unwraps a comparator's order of two keys at a line and column. */
static StdOrdering
doltgres_layout_tests_compare_keys(const DoltgresLayoutTableComparator *comparator, HostIoIo *io, const StdVec *a,
								   const StdVec *b, uint32 line, uint32 col)
{
	StdBytes	x = {a->ptr, a->len};
	StdBytes	y = {b->ptr, b->len};
	StdOrdering ordering;
	HostError	error;

	if (!doltgres_layout_TableComparator_compare_keys(comparator, io, x, y, &ordering, &error))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_LAYOUT_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
	return ordering;
}

/* doltgres_layout_tests_assert_ordering is an assert_eq! of two orderings at a line and column. */
static void
doltgres_layout_tests_assert_ordering(StdOrdering left, StdOrdering right, uint32 line, uint32 col)
{
	STD_ASSERT_EQ(line, col, left == right, &left, std_Ordering_Debug_fmt, &right, std_Ordering_Debug_fmt);
}

/*
 * test_doltgres_layout_tests_a_comparator_orders_compared_columns_by_their_function_closure_0
 * is the case's actor.
 */
static void
test_doltgres_layout_tests_a_comparator_orders_compared_columns_by_their_function_closure_0(void *captures)
{
	HostIoIo   *io = captures;
	static const uint32 sevens[] = {7, 7};
	static const uint32 ones[] = {1};
	static const uint32 zeros[] = {0};
	StdBytes	ab_value[] = {{(const uint8 *) "ab", 2}};
	StdBytes	b_value[] = {{(const uint8 *) "b", 1}};
	StdBytes	null_value[] = {{NULL, 0}};
	StdBytes	longer_value[] = {{(const uint8 *) "b", 1}, {(const uint8 *) "a", 1}};
	StdBytes	x_value[] = {{(const uint8 *) "x", 1}};
	StdBytes	y_value[] = {{(const uint8 *) "y", 1}};
	DoltgresLayoutTableComparator reversed = doltgres_layout_tests_comparator(sevens, lengthof(sevens));
	StdVec		ab = doltgres_layout_tests_key(&reversed, ab_value, lengthof(ab_value));
	StdVec		b = doltgres_layout_tests_key(&reversed, b_value, lengthof(b_value));
	StdVec		null;
	StdVec		longer;
	DoltgresLayoutTableComparator equal;
	StdVec		x;
	StdVec		y;
	DoltgresLayoutTableComparator raising;
	StdBytes	xb;
	StdBytes	yb;
	StdOrdering ordering;
	HostError	error;
	bool		ok;

	STD_ASSERT(605, 13, std_bytes_Ord_cmp(ab.ptr, ab.len, b.ptr, b.len) == STD_ORDERING_LESS, "ab < b");
	doltgres_layout_tests_assert_ordering(doltgres_layout_tests_compare_keys(&reversed, io, &ab, &b, 606, 59),
										  STD_ORDERING_GREATER, 606, 13);
	null = doltgres_layout_tests_key(&reversed, null_value, lengthof(null_value));
	doltgres_layout_tests_assert_ordering(doltgres_layout_tests_compare_keys(&reversed, io, &b, &null, 608, 61),
										  STD_ORDERING_LESS, 608, 13);
	longer = doltgres_layout_tests_key(&reversed, longer_value, lengthof(longer_value));
	doltgres_layout_tests_assert_ordering(doltgres_layout_tests_compare_keys(&reversed, io, &b, &longer, 610, 63),
										  STD_ORDERING_LESS, 610, 13);
	doltgres_layout_tests_assert_ordering(doltgres_layout_tests_compare_keys(&reversed, io, &longer, &longer, 611,
																			 68),
										  STD_ORDERING_EQUAL, 611, 13);
	equal = doltgres_layout_tests_comparator(ones, lengthof(ones));
	x = doltgres_layout_tests_key(&equal, x_value, lengthof(x_value));
	y = doltgres_layout_tests_key(&equal, y_value, lengthof(y_value));
	doltgres_layout_tests_assert_ordering(doltgres_layout_tests_compare_keys(&equal, io, &y, &x, 614, 55),
										  STD_ORDERING_GREATER, 614, 13);
	raising = doltgres_layout_tests_comparator(zeros, lengthof(zeros));
	xb = (StdBytes)
	{
		x.ptr, x.len
	};
	yb = (StdBytes)
	{
		y.ptr, y.len
	};
	ok = doltgres_layout_TableComparator_compare_keys(&raising, io, xb, yb, &ordering, &error);
	STD_ASSERT(616, 13, !ok && memcmp(host_Error_sqlstate(&error), "42883", 5) == 0,
			   "raising.compare_keys(io, &x, &y).is_err_and(|error| error.sqlstate() == b\"42883\")");
	host_Error_drop(&error);
	doltgres_layout_TableComparator_drop(&raising);
	std_vec_free_Checked(&x, DOLTGRES_LAYOUT_BYTES);
	std_vec_free_Checked(&y, DOLTGRES_LAYOUT_BYTES);
	doltgres_layout_TableComparator_drop(&equal);
	std_vec_free_Checked(&longer, DOLTGRES_LAYOUT_BYTES);
	std_vec_free_Checked(&null, DOLTGRES_LAYOUT_BYTES);
	std_vec_free_Checked(&ab, DOLTGRES_LAYOUT_BYTES);
	std_vec_free_Checked(&b, DOLTGRES_LAYOUT_BYTES);
	doltgres_layout_TableComparator_drop(&reversed);
}

/*
 * test_doltgres_layout_tests_a_comparator_orders_compared_columns_by_their_function
 * checks the storage design's II.6, a compared column: a comparator orders a
 * compared column by its class's function, here the reverse of the values'
 * bytes; a NULL after any value, by the key's marker, with no call; a key
 * that ends first before a longer one; a tie of the functions by the whole
 * keys' bytes, so only equal bytes compare equal; and a function's error as
 * the comparison's.
 */
void
test_doltgres_layout_tests_a_comparator_orders_compared_columns_by_their_function(void)
{
	SimcTestIo	test_io = simc_io();

	simc_as_actor(1, test_doltgres_layout_tests_a_comparator_orders_compared_columns_by_their_function_closure_0,
				  &test_io.io);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_DOLTGRES */

#endif							/* FRONTEND */
