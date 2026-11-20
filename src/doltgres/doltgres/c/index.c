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
 * Which btree indexes the fork stores (doltgres' index.rs, the btree text's
 * section 4): an index of the btree method whose table the fork stores, the
 * first answer needing no catalog read at all; and an index's description,
 * the owned record its inserts lend, from which an entry is formed from the
 * executor's values.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "access/transam.h"
#include "catalog/pg_class_d.h"
#include "doltgres/access/relation.h"
#include "doltgres/doltgres/index.h"
#include "doltgres/doltgres/relation.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/host/owned.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "pg_calls.h"

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "index.rs"

/* The element layouts of index's vectors. */
#define DOLTGRES_INDEX_COLUMN sizeof(AccessHeaptupleColumn), _Alignof(AccessHeaptupleColumn)
#define DOLTGRES_INDEX_FORM sizeof(doltgres_TupleBoolI16_t), _Alignof(doltgres_TupleBoolI16_t)
#define DOLTGRES_INDEX_USIZE sizeof(size_t), _Alignof(size_t)
#define DOLTGRES_INDEX_U32 sizeof(uint32), _Alignof(uint32)
#define DOLTGRES_INDEX_MADE sizeof(access_TupleOrderKeyTypeColumn_t), _Alignof(access_TupleOrderKeyTypeColumn_t)
#define DOLTGRES_INDEX_KEY_TYPE sizeof(AccessKeyKeyType), _Alignof(AccessKeyKeyType)
#define DOLTGRES_INDEX_COLLATED sizeof(doltgres_TupleOrderKeyTypeU32_t), _Alignof(doltgres_TupleOrderKeyTypeU32_t)
#define DOLTGRES_INDEX_COMPARED sizeof(StdVec), _Alignof(StdVec)

/*
 * INDOPTION_DESC and INDOPTION_NULLS_FIRST are the bits of a key column's
 * indoption that order it descending and put its NULLs first.
 */
#define DOLTGRES_INDEX_INDOPTION_DESC 1
#define DOLTGRES_INDEX_INDOPTION_NULLS_FIRST 2

/* DOLTGRES_INDEX_KEYLESS_KEY is the length of a keyless table's row key. */
#define DOLTGRES_INDEX_KEYLESS_KEY 16

/* doltgres_index_fork returns the fork's XX000 error of a message. */
static PgPgError
doltgres_index_fork(const char *message)
{
	StdString	str;

	std_string_from_Checked(&str, message, strlen(message));
	return pg_PgError_fork((const uint8 *) "XX000", &str);
}

/*
 * doltgres_index_ported reports whether the fork stores an index's entries
 * and gives it its routine: a relation of kind RELKIND_INDEX whose handler is
 * bthandler and whose table the fork stores, a table below
 * FirstNormalObjectId answering no with no read.
 */
bool
doltgres_index_ported(const PgMayRaise *raise, const PgCatalog *catalog, const PgRel *rel, bool *result,
					  PgPgError *error)
{
	uint32		table;
	DgClass		class;
	bool		twin;

	*result = false;
	if (pg_calls_rel_kind(rel) != RELKIND_INDEX || !pg_calls_rel_btree(rel))
		return true;
	table = pg_calls_index_table(rel);
	if (table < FirstNormalObjectId)
		return true;
	if (!pg_calls_class_row(raise, catalog, table, &class, error))
		return false;
	if (!access_relation_stored(table, (uint8) class.kind, doltgres_relation_rewrites(table, class.rewrite)))
		return true;
	if (!doltgres_relation_twin_table(raise, catalog, table, &twin, error))
		return false;
	*result = !twin;
	return true;
}

/*
 * doltgres_index_word returns the word of a by-value column's value as values
 * reads it, 0 for bytes that are no word.
 */
static uint64
doltgres_index_word(const uint8 *bytes, size_t len)
{
	uint64		word = 0;

	if (len == 8)
		for (int i = 0; i < 8; i++)
			word |= (uint64) bytes[i] << (8 * i);
	return word;
}

/* doltgres_index_most returns the most bytes a column's value takes: its length, or no bound for a variable length. */
static size_t
doltgres_index_most(const AccessHeaptupleColumn *column)
{
	return column->len > 0 ? (size_t) column->len : SIZE_MAX;
}

/* doltgres_index_made_drop frees a key column's key type, as a vector's element. */
static void
doltgres_index_made_drop(void *made)
{
	access_key_KeyType_drop(&((access_TupleOrderKeyTypeColumn_t *) made)->f1);
}

/* doltgres_index_key_type_drop frees a key type, as a vector's element. */
static void
doltgres_index_key_type_drop(void *key_type)
{
	access_key_KeyType_drop(key_type);
}

/* doltgres_index_collated_drop frees a comparator's input column's key type, as a vector's element. */
static void
doltgres_index_collated_drop(void *collated)
{
	access_key_KeyType_drop(&((doltgres_TupleOrderKeyTypeU32_t *) collated)->f1);
}

/*
 * doltgres_index_compared_closure_2 is compared's function: the comparison
 * function of an index's key column's class.
 */
static bool
doltgres_index_compared_closure_2(const void *captures, const PgMayRaise *raise, const PgCatalog *catalog, size_t at,
								  uint32 *result, PgPgError *error)
{
	return pg_calls_index_order_function(raise, captures, (int32) at, result, error);
}

/*
 * doltgres_index_compared_frame_t is compared's state across its pad: its
 * columns' key types and the comparator's input columns.
 */
typedef struct doltgres_index_compared_frame_t
{
	StdVec		key_types;
	bool		key_types_live;
	StdVec		columns;
	bool		columns_live;
	const PgMayRaise *raise;
	const PgCatalog *catalog;
	const PgRel *index;
	const		DgIndexColumn *read;
	size_t		read_len;
	StdArcInner **result;
	PgPgError  *error;
	bool		ok;
} doltgres_index_compared_frame_t;

/* doltgres_index_compared_pad frees compared's key types and columns. */
static void
doltgres_index_compared_pad(void *frame_)
{
	doltgres_index_compared_frame_t *frame = frame_;

	if (frame->key_types_live)
		std_vec_drop_Checked(&frame->key_types, DOLTGRES_INDEX_KEY_TYPE, doltgres_index_key_type_drop);
	if (frame->columns_live)
		std_vec_drop_Checked(&frame->columns, DOLTGRES_INDEX_COLLATED, doltgres_index_collated_drop);
	frame->key_types_live = frame->columns_live = false;
}

/*
 * doltgres_index_compared_body reads each key column's key type and, when one
 * is compared through its class, makes the comparator's columns.
 */
static void
doltgres_index_compared_body(void *frame_)
{
	doltgres_index_compared_frame_t *frame = frame_;
	bool		any = false;
	StdVec		compared;

	std_vec_with_capacity_Checked(&frame->key_types, frame->read_len, DOLTGRES_INDEX_KEY_TYPE);
	frame->key_types_live = true;
	for (size_t at = 0; at < frame->read_len; at++)
	{
		bool		default_class;
		AccessKeyKeyType key_type;

		if (!pg_calls_index_default_class(frame->raise, frame->catalog, frame->index, (int32) at, &default_class,
										  frame->error) ||
			!doltgres_layout_class_key_type(frame->raise, frame->catalog, frame->read[at].type,
											frame->read[at].collation, default_class, &key_type, frame->error))
			return;
		std_vec_push_Checked(&frame->key_types, &key_type, DOLTGRES_INDEX_KEY_TYPE);
	}
	for (size_t i = 0; i < frame->key_types.len && !any; i++)
		any = STD_VEC_AT(&frame->key_types, AccessKeyKeyType, i).inner.tag == ACCESS_KEY_INNER_COMPARED;
	if (!any)
	{
		*frame->result = NULL;
		frame->ok = true;
		return;
	}
	std_vec_with_capacity_Checked(&frame->columns, Min(frame->read_len, frame->key_types.len), DOLTGRES_INDEX_COLLATED);
	frame->columns_live = true;
	for (size_t i = 0; i < Min(frame->read_len, frame->key_types.len); i++)
	{
		doltgres_TupleOrderKeyTypeU32_t column;

		column.f0.descending = (frame->read[i].option & DOLTGRES_INDEX_INDOPTION_DESC) != 0;
		column.f0.nulls_first = (frame->read[i].option & DOLTGRES_INDEX_INDOPTION_NULLS_FIRST) != 0;
		column.f1 = STD_VEC_AT(&frame->key_types, AccessKeyKeyType, i);
		column.f2 = frame->read[i].collation;
		std_vec_push_Checked(&frame->columns, &column, DOLTGRES_INDEX_COLLATED);
	}
	std_vec_free_Checked(&frame->key_types, DOLTGRES_INDEX_KEY_TYPE);
	frame->key_types_live = false;
	frame->columns_live = false;
	if (!doltgres_layout_compared_columns(frame->raise, frame->catalog, &frame->columns,
										  doltgres_index_compared_closure_2, frame->index, &compared, frame->error))
		return;
	*frame->result = std_arc_new(&compared, DOLTGRES_INDEX_COMPARED);
	frame->ok = true;
}

/*
 * doltgres_index_compared returns an index's key columns as its tree's
 * comparator takes them (an Arc<Compared>), when one of them is compared
 * through its class, or none (NULL) when every one has a key of its own.
 */
static bool
doltgres_index_compared(const PgMayRaise *raise, const PgCatalog *catalog, const PgRel *index,
						const DgIndexColumn * read, size_t read_len, StdArcInner **result, PgPgError *error)
{
	doltgres_index_compared_frame_t frame = {.raise = raise, .catalog = catalog, .index = index, .read = read,
	.read_len = read_len, .result = result, .error = error};

	std_landing(&frame, doltgres_index_compared_body, doltgres_index_compared_pad);
	doltgres_index_compared_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_index_keyed reports whether an index has no tree of its own: it
 * is unique, has no expressions and no predicate, and its key columns,
 * operator classes, and collations are its table's key as the table's
 * storage recorded it.
 */
bool
doltgres_index_keyed(const PgMayRaise *raise, const PgCatalog *catalog, const PgRel *index, const TxnBaseKey *recorded,
					 bool *result, PgPgError *error)
{
	DgKeyColumn read[INDEX_MAX_KEYS] = {0};
	int32		count;
	const StdVec *columns;

	*result = false;
	if (recorded->tag != TXN_BASE_KEY_COLUMNS)
		return true;
	columns = &recorded->u.columns;
	if (!pg_calls_index_key_columns(raise, catalog, index, read, &count, error))
		return false;
	if (count < 0 || (size_t) count != columns->len)
		return true;
	std_range_to_check((size_t) count, INDEX_MAX_KEYS, RUST_FILE, 232, 34);
	*result = true;
	for (size_t i = 0; i < (size_t) count && *result; i++)
	{
		const TxnBaseKeyColumn *column = &STD_VEC_AT(columns, TxnBaseKeyColumn, i);

		*result = (uint16) (read[i].attnum < 0 ? -(int32) read[i].attnum : read[i].attnum) == column->attnum && read[i].class_ == column->class &&
			read[i].collation == column->collation;
	}
	return true;
}

/*
 * doltgres_index_describe_frame_t is describe's state across its pad: the
 * index's columns, their forms, its INCLUDE columns, its parts, its key
 * columns, its collations, its key, and its comparator's columns.
 */
typedef struct doltgres_index_describe_frame_t
{
	bool		columns_live;
	StdVec		columns;
	bool		forms_live;
	StdVec		forms;
	bool		includes_live;
	StdVec		includes;
	bool		parts_live;
	StdVec		parts;
	bool		key_columns_live;
	StdVec		key_columns;
	bool		collations_live;
	StdVec		collations;
	bool		key_live;
	AccessIndexIndexKey key;
	StdArcInner *compared;
	const PgMayRaise *raise;
	const PgCatalog *catalog;
	const PgRel *index;
	const access_TupleUsizeOrderKeyType_t *table_key;
	size_t		table_key_len;
	const TxnBaseKey *recorded;
	DoltgresIndexDescribed *result;
	PgPgError  *error;
	bool		ok;
} doltgres_index_describe_frame_t;

/* doltgres_index_compared_arc_drop drops an Arc<Compared>, or nothing for none. */
static void
doltgres_index_compared_arc_drop(StdArcInner *compared)
{
	if (compared != NULL)
		std_arc_drop(compared, DOLTGRES_INDEX_COMPARED, doltgres_layout_Compared_drop);
}

/* doltgres_index_describe_pad frees what describe holds. */
static void
doltgres_index_describe_pad(void *frame_)
{
	doltgres_index_describe_frame_t *frame = frame_;

	if (frame->columns_live)
		std_vec_free_Checked(&frame->columns, DOLTGRES_INDEX_COLUMN);
	if (frame->forms_live)
		std_vec_free_Checked(&frame->forms, DOLTGRES_INDEX_FORM);
	if (frame->includes_live)
		std_vec_free_Checked(&frame->includes, DOLTGRES_INDEX_COLUMN);
	if (frame->parts_live)
		std_vec_free_Checked(&frame->parts, DOLTGRES_INDEX_USIZE);
	if (frame->key_columns_live)
		std_vec_drop_Checked(&frame->key_columns, DOLTGRES_INDEX_MADE, doltgres_index_made_drop);
	if (frame->collations_live)
		std_vec_free_Checked(&frame->collations, DOLTGRES_INDEX_U32);
	if (frame->key_live)
		access_index_IndexKey_drop(&frame->key);
	doltgres_index_compared_arc_drop(frame->compared);
	frame->compared = NULL;
	frame->columns_live = frame->forms_live = frame->includes_live = frame->parts_live = false;
	frame->key_columns_live = frame->collations_live = frame->key_live = false;
}

/* doltgres_index_describe_body reads the index's columns and makes its key and description. */
static void
doltgres_index_describe_body(void *frame_)
{
	doltgres_index_describe_frame_t *frame = frame_;
	uint32		counts = pg_calls_index_counts(frame->index);
	size_t		keys = (size_t) (counts >> 16);
	size_t		all = (size_t) (counts & 0xffff);
	DgIndexColumn read[INDEX_MAX_KEYS] = {0};
	size_t		row_key;
	AccessIndexUnique unique;
	bool		keyed;
	DoltgresIndexDescribed *result = frame->result;

	pg_calls_index_columns(frame->index, read);
	std_vec_with_capacity_Checked(&frame->columns, all, DOLTGRES_INDEX_COLUMN);
	frame->columns_live = true;
	for (int32 at = 0; at < (int32) all; at++)
	{
		PgAttOption att = pg_Att_of(pg_calls_index_att(frame->index, at));
		AccessHeaptupleColumn column = {0};
		uint8		align;

		if (!att.has)
		{
			*frame->error = doltgres_index_fork("an index's descriptor lost an attribute");
			return;
		}
		std_index_check((size_t) at, INDEX_MAX_KEYS, RUST_FILE, 144, 21);
		column.typid = read[at].type;
		if (column.typid == pg_Att_type_id(att.att))
		{
			column.len = pg_Att_length(att.att);
			column.by_value = pg_Att_by_value(att.att);
		}
		else if (!doltgres_layout_storage(frame->raise, frame->catalog, column.typid, &column.len, &column.by_value,
										  &align, frame->error))
			return;
		column.align = pg_Att_align(att.att);
		if (!access_heaptuple_Storage_of(pg_calls_index_att_storage(frame->index, at), &column.storage, frame->error))
			return;
		std_vec_push_Checked(&frame->columns, &column, DOLTGRES_INDEX_COLUMN);
	}
	std_vec_with_capacity_Checked(&frame->forms, frame->columns.len, DOLTGRES_INDEX_FORM);
	frame->forms_live = true;
	for (size_t i = 0; i < frame->columns.len; i++)
	{
		const AccessHeaptupleColumn *column = &STD_VEC_AT(&frame->columns, AccessHeaptupleColumn, i);
		doltgres_TupleBoolI16_t form = {column->by_value, column->len};

		std_vec_push_Checked(&frame->forms, &form, DOLTGRES_INDEX_FORM);
	}
	std_vec_split_off(&frame->columns, keys, DOLTGRES_INDEX_COLUMN, &frame->includes, RUST_FILE,
					  160, 28);
	frame->includes_live = true;
	std_vec_with_capacity_Checked(&frame->parts, frame->includes.len, DOLTGRES_INDEX_USIZE);
	frame->parts_live = true;
	for (size_t i = 0; i < frame->includes.len; i++)
	{
		size_t		most = doltgres_index_most(&STD_VEC_AT(&frame->includes, AccessHeaptupleColumn, i));

		std_vec_push_Checked(&frame->parts, &most, DOLTGRES_INDEX_USIZE);
	}
	std_vec_with_capacity_Checked(&frame->key_columns, keys, DOLTGRES_INDEX_MADE);
	frame->key_columns_live = true;
	std_range_to_check(keys, INDEX_MAX_KEYS, RUST_FILE, 163, 22);
	std_vec_with_capacity_Checked(&frame->collations, keys, DOLTGRES_INDEX_U32);
	frame->collations_live = true;
	for (size_t i = 0; i < keys; i++)
		std_vec_push_Checked(&frame->collations, &read[i].collation, DOLTGRES_INDEX_U32);
	std_range_to_check(keys, INDEX_MAX_KEYS, RUST_FILE, 164, 35);
	for (size_t at = 0; at < Min(keys, frame->columns.len); at++)
	{
		access_TupleOrderKeyTypeColumn_t made;
		bool		default_class;

		made.f0.descending = (read[at].option & DOLTGRES_INDEX_INDOPTION_DESC) != 0;
		made.f0.nulls_first = (read[at].option & DOLTGRES_INDEX_INDOPTION_NULLS_FIRST) != 0;
		made.f2 = STD_VEC_AT(&frame->columns, AccessHeaptupleColumn, at);
		if (!pg_calls_index_default_class(frame->raise, frame->catalog, frame->index, (int32) at, &default_class,
										  frame->error) ||
			!doltgres_layout_class_key_type(frame->raise, frame->catalog, read[at].type, read[at].collation,
											default_class, &made.f1, frame->error))
			return;
		if (!access_key_is_exact(&made.f1))
		{
			size_t		most = doltgres_index_most(&made.f2);

			std_vec_push_Checked(&frame->parts, &most, DOLTGRES_INDEX_USIZE);
		}
		std_vec_push_Checked(&frame->key_columns, &made, DOLTGRES_INDEX_MADE);
	}
	std_vec_free_Checked(&frame->columns, DOLTGRES_INDEX_COLUMN);
	frame->columns_live = false;
	if (frame->table_key_len == 0)
		row_key = DOLTGRES_INDEX_KEYLESS_KEY;
	else
	{
		AccessKeyKeyTypes key_types = {frame->table_key, frame->table_key_len, sizeof(access_TupleUsizeOrderKeyType_t),
		offsetof(access_TupleUsizeOrderKeyType_t, f2)};

		row_key = access_key_table_key_most(key_types);
	}
	unique = access_index_Unique_of(pg_calls_index_unique(frame->index), pg_calls_index_immediate(frame->index),
									pg_calls_index_nulls_not_distinct(frame->index));
	access_index_IndexKey_new(&frame->key_columns, &frame->includes, unique, frame->table_key_len == 0, row_key,
							  frame->parts.ptr, frame->parts.len, &frame->key);
	frame->key_columns_live = frame->includes_live = false;
	frame->key_live = true;
	if (!doltgres_index_keyed(frame->raise, frame->catalog, frame->index, frame->recorded, &keyed, frame->error))
		return;
	std_range_to_check(keys, INDEX_MAX_KEYS, RUST_FILE, 183, 53);
	if (!doltgres_index_compared(frame->raise, frame->catalog, frame->index, read, keys, &frame->compared,
								 frame->error))
		return;
	result->key = frame->key;
	result->forms = frame->forms;
	result->keys = keys;
	result->keyless = frame->table_key_len == 0;
	result->keyed = keyed;
	result->unique = unique != ACCESS_INDEX_UNIQUE_NO;
	result->collations = frame->collations;
	result->compared = frame->compared;
	result->records = pg_calls_index_recorded(frame->index);
	frame->key_live = frame->forms_live = frame->collations_live = false;
	frame->compared = NULL;
	frame->ok = true;
}

/*
 * doltgres_index_describe returns an index's description from its relcache
 * entry, its table's recorded key, and its keyed layout: each key column's
 * order from rd_indoption and key type under its collation and operator
 * class, each column's stored form, its uniqueness from pg_index, the bound
 * its definition gives, and whether it has no tree of its own.
 */
bool
doltgres_index_describe(const PgMayRaise *raise, const PgCatalog *catalog, const PgRel *index,
						const access_TupleUsizeOrderKeyType_t *table_key, size_t table_key_len,
						const TxnBaseKey *recorded, DoltgresIndexDescribed *result, PgPgError *error)
{
	doltgres_index_describe_frame_t frame = {.raise = raise, .catalog = catalog, .index = index,
		.table_key = table_key, .table_key_len = table_key_len, .recorded = recorded, .result = result,
	.error = error};

	std_landing(&frame, doltgres_index_describe_body, doltgres_index_describe_pad);
	doltgres_index_describe_pad(&frame);
	return frame.ok;
}

/* doltgres_index_Described_drop frees what an index's description holds. */
void
doltgres_index_Described_drop(DoltgresIndexDescribed *self)
{
	access_index_IndexKey_drop(&self->key);
	std_vec_free_Checked(&self->forms, DOLTGRES_INDEX_FORM);
	std_vec_free_Checked(&self->collations, DOLTGRES_INDEX_U32);
	doltgres_index_compared_arc_drop(self->compared);
	self->compared = NULL;
}

/*
 * doltgres_index_Described_keyless reports whether the index's table is
 * keyless, whose entries count the row's copies.
 */
bool
doltgres_index_Described_keyless(const DoltgresIndexDescribed *self)
{
	return self->keyless;
}

/* doltgres_index_Described_keyed reports whether the index has no tree of its own, its entries its table's rows. */
bool
doltgres_index_Described_keyed(const DoltgresIndexDescribed *self)
{
	return self->keyed;
}

/*
 * doltgres_index_Described_unique reports whether the index is unique and
 * checked immediately, so two rows' equal keys collide.
 */
bool
doltgres_index_Described_unique(const DoltgresIndexDescribed *self)
{
	return self->unique;
}

/*
 * doltgres_index_Described_records reports whether the index keeps a record of
 * each entry, its entries needing user code to form.
 */
bool
doltgres_index_Described_records(const DoltgresIndexDescribed *self)
{
	return self->records;
}

/*
 * doltgres_index_Described_null_key reports whether a row's values, as values
 * read them, hold a NULL in a key column.
 */
bool
doltgres_index_Described_null_key(const DoltgresIndexDescribed *self, const access_OptionVecU8_t *read,
								  size_t read_len)
{
	std_range_to_check(self->keys, read_len, RUST_FILE, 265, 9);
	for (size_t i = 0; i < self->keys; i++)
		if (!read[i].has)
			return true;
	return false;
}

/* doltgres_index_Described_forms returns each of the index's columns' stored form, its key columns first. */
const doltgres_TupleBoolI16_t *
doltgres_index_Described_forms(const DoltgresIndexDescribed *self, size_t *result_len)
{
	*result_len = self->forms.len;
	return self->forms.ptr;
}

/*
 * doltgres_index_Described_scan_bound returns the bound a scan's tree cuts the
 * index's key columns at: its own tree's, or its table's for an index with no
 * tree of its own.
 */
size_t
doltgres_index_Described_scan_bound(const DoltgresIndexDescribed *self)
{
	return self->keyed ? access_index_IndexKey_table_bound(&self->key) : access_index_IndexKey_bound(&self->key);
}

/*
 * doltgres_index_Described_order returns the order of the index's tree, taking
 * its probes (NULL for none): by its keys' bytes, or by its comparator.
 */
void
doltgres_index_Described_order(const DoltgresIndexDescribed *self, StdArcInner *probes, TreesKeyOrder *result)
{
	if (self->compared != NULL)
	{
		doltgres_layout_index_order(std_arc_clone(self->compared), probes, result);
		return;
	}
	if (probes != NULL)
		std_arc_drop(probes, sizeof(HostLockIoMutexVecProbe), _Alignof(HostLockIoMutexVecProbe),
					 doltgres_layout_Probes_drop);
	memset(result, 0, sizeof(*result));
	result->tag = TREES_KEY_ORDER_BYTES;
}

/* doltgres_index_Described_compared reports whether the index's tree is ordered by a comparator. */
bool
doltgres_index_Described_compared(const DoltgresIndexDescribed *self)
{
	return self->compared != NULL;
}

/* doltgres_index_Described_entry_key returns how the index's entries are made, which a scan reads them by. */
const AccessIndexIndexKey *
doltgres_index_Described_entry_key(const DoltgresIndexDescribed *self)
{
	return &self->key;
}

/* doltgres_index_Described_scan_columns returns the index's key columns as a scan reads them (a Vec<KeyColumn>). */
void
doltgres_index_Described_scan_columns(const DoltgresIndexDescribed *self, StdVec *result)
{
	AccessIndexKeyColumns columns = access_index_IndexKey_key_columns(&self->key);
	access_TupleOrderKeyTypeColumnRef_t column;

	std_vec_new(result, sizeof(AccessNbtsearchKeyColumn), _Alignof(AccessNbtsearchKeyColumn));
	for (size_t i = 0; i < self->collations.len && access_index_KeyColumns_Iterator_next(&columns, &column); i++)
	{
		AccessNbtsearchKeyColumn scan = {column.f0, column.f1, STD_VEC_AT(&self->collations, uint32, i),
		column.f2->len, column.f2->by_value};

		std_vec_push_Checked(result, &scan, sizeof(AccessNbtsearchKeyColumn), _Alignof(AccessNbtsearchKeyColumn));
	}
}

/* DOLTGRES_INDEX_READ is the element layout of a row's values as values reads them, a Vec<Option<Vec<u8>>>. */
#define DOLTGRES_INDEX_READ sizeof(access_OptionVecU8_t), _Alignof(access_OptionVecU8_t)

/* doltgres_index_read_drop frees a value's bytes, as a vector's element. */
static void
doltgres_index_read_drop(void *read)
{
	access_OptionVecU8_t *value = read;

	if (value->has)
		std_vec_free_Checked(&value->bytes, 1, 1);
}

/* doltgres_index_values_drop frees a row's values as values reads them. */
void
doltgres_index_values_drop(StdVec *values)
{
	std_vec_drop_Checked(values, DOLTGRES_INDEX_READ, doltgres_index_read_drop);
}

/* doltgres_index_values_frame_t is values' state across its pad: the values it reads. */
typedef struct doltgres_index_values_frame_t
{
	bool		read_live;
	StdVec		read;
	const DoltgresIndexDescribed *self;
	const PgCatalogCaps *caps;
	const PgDatums *values;
	const PgIsNull *isnull;
	PgPgError  *error;
	bool		ok;
} doltgres_index_values_frame_t;

/* doltgres_index_values_pad frees the values values reads. */
static void
doltgres_index_values_pad(void *frame_)
{
	doltgres_index_values_frame_t *frame = frame_;

	if (frame->read_live)
		doltgres_index_values_drop(&frame->read);
	frame->read_live = false;
}

/* doltgres_index_values_body reads each column's value. */
static void
doltgres_index_values_body(void *frame_)
{
	doltgres_index_values_frame_t *frame = frame_;
	const StdVec *forms = &frame->self->forms;
	int32		at = 0;

	std_vec_with_capacity_Checked(&frame->read, forms->len, DOLTGRES_INDEX_READ);
	frame->read_live = true;
	for (size_t k = 0;; k++)
	{
		int32		taken = at;
		const doltgres_TupleBoolI16_t *form;
		access_OptionVecU8_t value = {0};

		at = std_forward_i32_Checked(at, 1);
		if (k >= forms->len)
			break;
		form = &STD_VEC_AT(forms, doltgres_TupleBoolI16_t, k);
		if (!pg_calls_index_datum_null(frame->isnull, taken))
		{
			value.has = true;
			if (form->f0)
			{
				uint64		word = pg_calls_index_datum_word(frame->values, taken);
				uint8		bytes[8];

				for (int i = 0; i < 8; i++)
					bytes[i] = (uint8) (word >> (8 * i));
				std_slice_to_vec_Checked(&value.bytes, bytes, 8, 1, 1);
			}
			else
			{
				const uint8 *bytes;
				size_t		len;

				if (!pg_calls_index_datum_bytes(pg_CatalogCaps_may_raise(frame->caps),
												pg_CatalogCaps_catalog(frame->caps), frame->values, taken,
												(int32) form->f1, &bytes, &len, frame->error))
					return;
				std_slice_to_vec_Checked(&value.bytes, bytes, len, 1, 1);
			}
		}
		std_vec_push_Checked(&frame->read, &value, DOLTGRES_INDEX_READ);
	}
	frame->ok = true;
}

/*
 * doltgres_index_Described_values returns the executor's values of a row's
 * entry (a Vec<Option<Vec<u8>>>), each column's as forming takes it, a
 * by-value word's bytes or a by-reference value's bytes read whole, or none
 * for a NULL.
 */
bool
doltgres_index_Described_values(const DoltgresIndexDescribed *self, const PgCatalogCaps *caps, const PgDatums *values,
								const PgIsNull *isnull, StdVec *result, PgPgError *error)
{
	doltgres_index_values_frame_t frame = {.self = self, .caps = caps, .values = values, .isnull = isnull,
	.error = error};

	std_landing(&frame, doltgres_index_values_body, doltgres_index_values_pad);
	if (frame.ok)
	{
		*result = frame.read;
		frame.read_live = false;
	}
	doltgres_index_values_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_index_given_frame_t is key's and formed's state across their pads:
 * the key columns' values, originals, and INCLUDE values they form, and the
 * entry's key.
 */
typedef struct doltgres_index_given_frame_t
{
	bool		given_live;
	StdVec		given;
	bool		originals_live;
	StdVec		originals;
	bool		includes_live;
	StdVec		includes;
	bool		key_live;
	AccessIndexEntryKey key;
	const DoltgresIndexDescribed *self;
	AccessKeySortKeys sorter;
	const access_OptionVecU8_t *read;
	size_t		read_len;
	StdBytes	row_key;
	AccessIndexEntryKey *key_result;
	DoltgresIndexFormed *formed_result;
	PgPgError  *error;
	bool		ok;
} doltgres_index_given_frame_t;

/* doltgres_index_given_pad frees what key and formed hold. */
static void
doltgres_index_given_pad(void *frame_)
{
	doltgres_index_given_frame_t *frame = frame_;

	if (frame->given_live)
		std_vec_free_Checked(&frame->given, sizeof(access_OptionValue_t), _Alignof(access_OptionValue_t));
	if (frame->originals_live)
		std_vec_free_Checked(&frame->originals, sizeof(AccessHeaptupleSet), _Alignof(AccessHeaptupleSet));
	if (frame->includes_live)
		std_vec_free_Checked(&frame->includes, sizeof(AccessHeaptupleSet), _Alignof(AccessHeaptupleSet));
	if (frame->key_live)
		access_index_EntryKey_drop(&frame->key);
	frame->given_live = frame->originals_live = frame->includes_live = frame->key_live = false;
}

/* doltgres_index_Described_key_body encodes the key columns' values under the row key. */
static bool
doltgres_index_Described_key_body(doltgres_index_given_frame_t *frame, AccessIndexEntryKey *result)
{
	const DoltgresIndexDescribed *self = frame->self;
	AccessKeyNames names = {&DOLTGRES_LAYOUT_BLOB_NAMES_NAMES_OPS, NULL};
	bool		ok;

	std_range_to_check(self->keys, self->forms.len, RUST_FILE, 346, 21);
	std_vec_with_capacity_Checked(&frame->given, Min(self->keys, frame->read_len), sizeof(access_OptionValue_t),
								  _Alignof(access_OptionValue_t));
	frame->given_live = true;
	for (size_t i = 0; i < Min(self->keys, frame->read_len); i++)
	{
		const doltgres_TupleBoolI16_t *form = &STD_VEC_AT(&self->forms, doltgres_TupleBoolI16_t, i);
		const access_OptionVecU8_t *value = &frame->read[i];
		access_OptionValue_t given = {value->has};

		if (value->has && form->f0)
		{
			given.value.tag = ACCESS_KEY_VALUE_WORD;
			given.value.u.word = doltgres_index_word(value->bytes.ptr, value->bytes.len);
		}
		else if (value->has)
		{
			given.value.tag = ACCESS_KEY_VALUE_BYTES;
			given.value.u.bytes.ptr = value->bytes.ptr;
			given.value.u.bytes.len = value->bytes.len;
		}
		std_vec_push_Checked(&frame->given, &given, sizeof(access_OptionValue_t), _Alignof(access_OptionValue_t));
	}
	ok = access_index_IndexKey_encode(&self->key, frame->sorter, names, frame->given.ptr, frame->given.len,
									  frame->row_key.ptr, frame->row_key.len, result, frame->error);
	std_vec_free_Checked(&frame->given, sizeof(access_OptionValue_t), _Alignof(access_OptionValue_t));
	frame->given_live = false;
	return ok;
}

/* doltgres_index_key_landing is key's body under its landing. */
static void
doltgres_index_key_landing(void *frame_)
{
	doltgres_index_given_frame_t *frame = frame_;

	frame->ok = doltgres_index_Described_key_body(frame, frame->key_result);
}

/*
 * doltgres_index_Described_key returns the key of a row's entry from its
 * values, as values read them, under its row key: each key column cut at the
 * index's bound, then the row key.
 */
bool
doltgres_index_Described_key(const DoltgresIndexDescribed *self, AccessKeySortKeys sorter,
							 const access_OptionVecU8_t *read, size_t read_len, const uint8 *row_key,
							 size_t row_key_len, AccessIndexEntryKey *result, PgPgError *error)
{
	doltgres_index_given_frame_t frame = {.self = self, .sorter = sorter, .read = read, .read_len = read_len,
	.row_key = {row_key, row_key_len}, .key_result = result, .error = error};

	std_landing(&frame, doltgres_index_key_landing, doltgres_index_given_pad);
	doltgres_index_given_pad(&frame);
	return frame.ok;
}

/* doltgres_index_Described_formed_closure_0 is formed's set: a value as read gives it, as forming takes it. */
static AccessHeaptupleSet
doltgres_index_Described_formed_closure_0(const doltgres_TupleBoolI16_t *form, const access_OptionVecU8_t *value)
{
	AccessHeaptupleSet set = {.tag = ACCESS_HEAPTUPLE_SET_NULL};

	if (!value->has)
		return set;
	if (form->f0)
	{
		set.tag = ACCESS_HEAPTUPLE_SET_WORD;
		set.u.word = doltgres_index_word(value->bytes.ptr, value->bytes.len);
		return set;
	}
	if (form->f1 == -1)
	{
		set.tag = ACCESS_HEAPTUPLE_SET_VARLENA;
		set.u.varlena.ptr = value->bytes.ptr;
		set.u.varlena.len = value->bytes.len;
		return set;
	}
	set.tag = ACCESS_HEAPTUPLE_SET_FIXED;
	set.u.fixed.ptr = value->bytes.ptr;
	set.u.fixed.len = value->bytes.len;
	return set;
}

/*
 * doltgres_index_formed_sets pushes formed's set of each form and value from
 * places on, as a zip of the two gives them.
 */
static void
doltgres_index_formed_sets(StdVec *sets, const StdVec *forms, size_t from, const access_OptionVecU8_t *read,
						   size_t read_len)
{
	std_vec_new(sets, sizeof(AccessHeaptupleSet), _Alignof(AccessHeaptupleSet));
	for (size_t i = 0; from + i < forms->len && i < read_len; i++)
	{
		AccessHeaptupleSet set = doltgres_index_Described_formed_closure_0(&STD_VEC_AT(forms, doltgres_TupleBoolI16_t,
																					   from + i), &read[i]);

		std_vec_push_Checked(sets, &set, sizeof(AccessHeaptupleSet), _Alignof(AccessHeaptupleSet));
	}
}

/* doltgres_index_formed_landing forms the entry's key, its originals and INCLUDE values, and its value's placing. */
static void
doltgres_index_formed_landing(void *frame_)
{
	doltgres_index_given_frame_t *frame = frame_;
	const DoltgresIndexDescribed *self = frame->self;

	if (!doltgres_index_Described_key_body(frame, &frame->key))
		return;
	frame->key_live = true;
	std_range_to_check(self->keys, self->forms.len, RUST_FILE, 369, 39);
	doltgres_index_formed_sets(&frame->originals, &self->forms, 0, frame->read, Min(self->keys, frame->read_len));
	frame->originals_live = true;
	std_range_from_check(self->keys, self->forms.len, RUST_FILE, 370, 38);
	std_range_from_check(self->keys, frame->read_len, RUST_FILE, 370, 74);
	doltgres_index_formed_sets(&frame->includes, &self->forms, self->keys, frame->read + self->keys,
							   frame->read_len - self->keys);
	frame->includes_live = true;
	if (!access_index_IndexKey_value(&self->key, &frame->key, frame->row_key.ptr, frame->row_key.len,
									 frame->originals.ptr, frame->originals.len, frame->includes.ptr,
									 frame->includes.len, &frame->formed_result->placing, frame->error))
		return;
	frame->formed_result->key = frame->key;
	frame->key_live = false;
	frame->ok = true;
}

/*
 * doltgres_index_Described_formed returns the entry of a row's values, as
 * values read them, under its row key: its key and the placing of its value,
 * which borrows the values.
 */
bool
doltgres_index_Described_formed(const DoltgresIndexDescribed *self, AccessKeySortKeys sorter,
								const access_OptionVecU8_t *read, size_t read_len, const uint8 *row_key,
								size_t row_key_len, DoltgresIndexFormed *result, PgPgError *error)
{
	doltgres_index_given_frame_t frame = {.self = self, .sorter = sorter, .read = read, .read_len = read_len,
	.row_key = {row_key, row_key_len}, .formed_result = result, .error = error};

	std_landing(&frame, doltgres_index_formed_landing, doltgres_index_given_pad);
	doltgres_index_given_pad(&frame);
	return frame.ok;
}

/* doltgres_index_Described_drop_glue is Described's drop glue, an Arc<Described>'s value's. */
static void
doltgres_index_Described_drop_glue(void *described)
{
	doltgres_index_Described_drop(described);
}

/* doltgres_index_Inserting_drop is Inserting's drop glue: its description's Arc. */
static void
doltgres_index_Inserting_drop(void *record)
{
	std_arc_drop(((DoltgresIndexInserting *) record)->f0, sizeof(DoltgresIndexDescribed),
				 _Alignof(DoltgresIndexDescribed), doltgres_index_Described_drop_glue);
}

/* DOLTGRES_INDEX_INSERTING is INSERTING, the backend's inserts' descriptions. */
HostOwnedTable DOLTGRES_INDEX_INSERTING = HOST_OWNED_TABLE_NEW;

/* doltgres_index_Inserting_Owned_table is Inserting's Owned::table, INSERTING. */
static HostOwnedTable *
doltgres_index_Inserting_Owned_table(void)
{
	return &DOLTGRES_INDEX_INSERTING;
}

/* Inserting's Table, host's owned Table<Inserting>, with its kind's glue. */
#define HOST_OWNED_SCOPE extern
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_index_Inserting
#define HOST_OWNED_RS DoltgresIndexInserting
#define HOST_OWNED_R DoltgresIndexInserting
#define HOST_OWNED_R_DROP doltgres_index_Inserting_drop
#define HOST_OWNED_KIND 3
#define HOST_OWNED_TABLE_FN doltgres_index_Inserting_Owned_table
#include "doltgres/host/owned_template.h"

/* Inserting's Table's with at an Arc<Described>. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_index_Inserting
#define HOST_OWNED_RS DoltgresIndexInserting
#define HOST_OWNED_R DoltgresIndexInserting
#define HOST_OWNED_R_DROP doltgres_index_Inserting_drop
#define HOST_OWNED_KIND 3
#define HOST_OWNED_TABLE_FN doltgres_index_Inserting_Owned_table
#define HOST_OWNED_WITH_TS Arc
#define HOST_OWNED_WITH_T StdArcInner *
#include "doltgres/host/owned_template.h"

/* doltgres_index_inserting_closure_0 is inserting's lend: a clone of the record's Arc. */
static bool
doltgres_index_inserting_closure_0(void *captures, HostIoIo *io, DoltgresIndexInserting *inserting,
								   StdArcInner **result, HostError *error)
{
	*result = std_arc_clone(inserting->f0);
	return true;
}

/*
 * doltgres_index_inserting returns the description an insert's handle names
 * (an Arc<Described>), cloning the record's Arc, since a row's insert forms
 * its entry outside the record's lend.
 */
bool
doltgres_index_inserting(HostIoIo *io, HostOwnedHandle handle, StdArcInner **result, HostError *error)
{
	return host_owned_TableDoltgresIndexInserting_withArc(&DOLTGRES_INDEX_INSERTING, io, handle,
														  doltgres_index_inserting_closure_0, NULL, NULL, result,
														  error);
}

/* DOLTGRES_INDEX_LISTED_ELEM and DOLTGRES_INDEX_KEPT_ELEM are the element layouts of LISTED's and UNCHANGED's lists. */
#define DOLTGRES_INDEX_LISTED_ELEM sizeof(DoltgresIndexListed), _Alignof(DoltgresIndexListed)
#define DOLTGRES_INDEX_KEPT_ELEM sizeof(doltgres_TupleU32U64U64U32_t), _Alignof(doltgres_TupleU32U64U64U32_t)

/* doltgres_index_Listed_drop is Listed's drop glue: its indexes' list. */
static void
doltgres_index_Listed_drop(void *listed)
{
	std_vec_free_Checked(&((DoltgresIndexListed *) listed)->indexes, DOLTGRES_INDEX_U32);
}

/* doltgres_index_VecListed_Default_default is Vec<Listed>'s Default: an empty list. */
static void
doltgres_index_VecListed_Default_default(StdVec *self)
{
	std_vec_new(self, DOLTGRES_INDEX_LISTED_ELEM);
}

/* doltgres_index_VecListed_drop is Vec<Listed>'s drop glue. */
static void
doltgres_index_VecListed_drop(void *value)
{
	std_vec_drop_Checked(value, DOLTGRES_INDEX_LISTED_ELEM, doltgres_index_Listed_drop);
}

/* doltgres_index_VecKept_Default_default is Vec<Kept>'s Default: an empty list. */
static void
doltgres_index_VecKept_Default_default(StdVec *self)
{
	std_vec_new(self, DOLTGRES_INDEX_KEPT_ELEM);
}

/* doltgres_index_VecKept_drop is Vec<Kept>'s drop glue. */
static void
doltgres_index_VecKept_drop(void *value)
{
	std_vec_free_Checked(value, DOLTGRES_INDEX_KEPT_ELEM);
}

/* taken at Vec<Listed> and IoMutex<Vec<Listed>>, which index.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecListed
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) doltgres_index_VecListed_Default_default(place)
#define HOST_LOCK_TAKEN_DROP(place) doltgres_index_VecListed_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_INDEX_LISTED_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecListed
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) doltgres_index_VecListed_Default_default(place)
#include "doltgres/host/lock_template.h"

/* taken at Vec<Kept> and IoMutex<Vec<Kept>>, which index.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecKept
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) doltgres_index_VecKept_Default_default(place)
#define HOST_LOCK_TAKEN_DROP(place) doltgres_index_VecKept_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_INDEX_KEPT_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecKept
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) doltgres_index_VecKept_Default_default(place)
#include "doltgres/host/lock_template.h"

/* doltgres_index_IoMutexVecListed_drop is IoMutex<Vec<Listed>>'s drop glue: its value's. */
static void
doltgres_index_IoMutexVecListed_drop(void *value)
{
	doltgres_index_VecListed_drop(&((HostLockIoMutexVecListed *) value)->value);
}

/* doltgres_index_IoMutexVecKept_drop is IoMutex<Vec<Kept>>'s drop glue: its value's. */
static void
doltgres_index_IoMutexVecKept_drop(void *value)
{
	doltgres_index_VecKept_drop(&((HostLockIoMutexVecKept *) value)->value);
}

/* DOLTGRES_INDEX_LISTED_TYPE and DOLTGRES_INDEX_KEPT_TYPE are the two IoMutexes' sizes, Defaults, and drops. */
static const StdDefaultType DOLTGRES_INDEX_LISTED_TYPE = {sizeof(HostLockIoMutexVecListed),
_Alignof(HostLockIoMutexVecListed), host_lock_IoMutexVecListed_Default_default, doltgres_index_IoMutexVecListed_drop};
static const StdDefaultType DOLTGRES_INDEX_KEPT_TYPE = {sizeof(HostLockIoMutexVecKept),
_Alignof(HostLockIoMutexVecKept), host_lock_IoMutexVecKept_Default_default, doltgres_index_IoMutexVecKept_drop};

/* DOLTGRES_INDEX_LISTED is LISTED: the backend's list of its removals of old entries, by table. */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecListed process;
}			DOLTGRES_INDEX_LISTED = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* DOLTGRES_INDEX_UNCHANGED is UNCHANGED: the indexes whose entry the backend's last update kept. */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecKept process;
}			DOLTGRES_INDEX_UNCHANGED = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* DoltgresIndexOp is an operation on a backend list under its lock, with its captures. */
typedef void (*DoltgresIndexOp) (StdVec *list, void *captures);

/*
 * doltgres_index_locked_frame_t is a locked operation's state across its pad:
 * the backend's list and its guard, each while live.
 */
typedef struct doltgres_index_locked_frame_t
{
	HostIoIo   *io;
	HostLocalBackendLocal *backend;
	const StdDefaultType *type;
	const HostLockType *lock_type;
	DoltgresIndexOp op;
	void	   *captures;
	HostLocalLocal local;
	bool		local_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
} doltgres_index_locked_frame_t;

/* doltgres_index_locked_body takes the backend's list and its lock, then runs the operation. */
static void
doltgres_index_locked_body(void *frame_)
{
	doltgres_index_locked_frame_t *frame = frame_;
	HostLockIoMutex *mutex;

	frame->local = host_local_BackendLocal_get(frame->backend, frame->type);
	frame->local_live = true;
	mutex = (HostLockIoMutex *) host_local_Local_Deref_deref(&frame->local, frame->type);
	frame->guard = host_lock_IoMutex_lock(mutex, frame->lock_type, &frame->io->held);
	frame->guard_live = true;
	frame->op(host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard), frame->captures);
}

/* doltgres_index_locked_pad releases what a locked operation holds: its guard, then the backend's list. */
static void
doltgres_index_locked_pad(void *frame_)
{
	doltgres_index_locked_frame_t *frame = frame_;

	if (frame->guard_live)
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	if (frame->local_live)
		host_local_Local_drop(&frame->local, frame->type);
	frame->guard_live = frame->local_live = false;
}

/* doltgres_index_listed_locked runs an operation on LISTED under its lock, as LISTED.get().lock(io) holds it. */
static void
doltgres_index_listed_locked(HostIoIo *io, DoltgresIndexOp op, void *captures)
{
	doltgres_index_locked_frame_t frame = {.io = io, .backend = &DOLTGRES_INDEX_LISTED.local,
		.type = &DOLTGRES_INDEX_LISTED_TYPE, .lock_type = &DOLTGRES_INDEX_LISTED_LOCK_TYPE, .op = op,
	.captures = captures};

	std_landing(&frame, doltgres_index_locked_body, doltgres_index_locked_pad);
	doltgres_index_locked_pad(&frame);
}

/*
 * doltgres_index_unchanged_locked runs an operation on UNCHANGED under its
 * lock, as UNCHANGED.get().lock(io) holds it.
 */
static void
doltgres_index_unchanged_locked(HostIoIo *io, DoltgresIndexOp op, void *captures)
{
	doltgres_index_locked_frame_t frame = {.io = io, .backend = &DOLTGRES_INDEX_UNCHANGED.local,
		.type = &DOLTGRES_INDEX_KEPT_TYPE, .lock_type = &DOLTGRES_INDEX_KEPT_LOCK_TYPE, .op = op,
	.captures = captures};

	std_landing(&frame, doltgres_index_locked_body, doltgres_index_locked_pad);
	doltgres_index_locked_pad(&frame);
}

/*
 * doltgres_index_in_drop runs an operation on LISTED under its lock taken in a
 * drop, when the backend has made the list and its lock is free.
 */
static void
doltgres_index_in_drop(DoltgresIndexOp op, void *captures)
{
	HostIoInDrop dropping = host_io_InDrop_new();
	HostLocalLocal local;
	StdRwLockWriteGuard guard;

	if (!host_local_BackendLocal_existing(&DOLTGRES_INDEX_LISTED.local, &local))
		return;
	if (host_lock_IoMutex_lock_in_drop((HostLockIoMutex *) host_local_Local_Deref_deref(&local,
																						&DOLTGRES_INDEX_LISTED_TYPE),
									   &dropping, &guard))
	{
		op(guard.data, captures);
		std_RwLockWriteGuard_drop(&guard);
	}
	host_local_Local_drop(&local, &DOLTGRES_INDEX_LISTED_TYPE);
}

/* doltgres_index_unchanged_t is unchanged's captures: the kept entry it looks for, and whether it found it. */
typedef struct doltgres_index_unchanged_t
{
	doltgres_TupleU32U64U64U32_t wanted;
	bool		found;
} doltgres_index_unchanged_t;

/*
 * doltgres_index_unchanged_closure_0 is unchanged's work under UNCHANGED's
 * lock: the kept entry it matches taken out of the list.
 */
static void
doltgres_index_unchanged_closure_0(StdVec *kept, void *captures)
{
	doltgres_index_unchanged_t *looking = captures;

	for (size_t at = 0; at < kept->len; at++)
	{
		doltgres_TupleU32U64U64U32_t *entry = &STD_VEC_AT(kept, doltgres_TupleU32U64U64U32_t, at);

		if (entry->f0 == looking->wanted.f0 && entry->f1 == looking->wanted.f1 && entry->f2 == looking->wanted.f2 &&
			entry->f3 == looking->wanted.f3)
		{
			doltgres_TupleU32U64U64U32_t removed;

			std_vec_swap_remove(kept, at, &removed, DOLTGRES_INDEX_KEPT_ELEM);
			looking->found = true;
			return;
		}
	}
}

/*
 * doltgres_index_unchanged reports whether an update by a writer kept an
 * index's entry for its new row's tuple id, taking it out of the list; a
 * later insert at that tuple id has another command or subtransaction and
 * never matches.
 */
bool
doltgres_index_unchanged(HostIoIo *io, uint32 index, uint64 tuple, const TxnMemtableVersionWriter *writer)
{
	doltgres_index_unchanged_t looking = {{index, tuple, writer->xid, writer->command}};

	doltgres_index_unchanged_locked(io, doltgres_index_unchanged_closure_0, &looking);
	return looking.found;
}

/* DOLTGRES_INDEX_INDEXED is the element layout of a removal's indexes, a Vec<(u32, Described)>. */
#define DOLTGRES_INDEX_INDEXED sizeof(doltgres_TupleU32Described_t), _Alignof(doltgres_TupleU32Described_t)

/* doltgres_index_indexed_drop frees an index's description, as a vector's element. */
static void
doltgres_index_indexed_drop(void *indexed)
{
	doltgres_index_Described_drop(&((doltgres_TupleU32Described_t *) indexed)->f1);
}

/* doltgres_index_VecIndexed_drop is Vec<(u32, Described)>'s drop glue, an Arc's value's. */
static void
doltgres_index_VecIndexed_drop(void *indexes)
{
	std_vec_drop_Checked(indexes, DOLTGRES_INDEX_INDEXED, doltgres_index_indexed_drop);
}

/* doltgres_index_indexes_arc_drop drops an Arc<Vec<(u32, Described)>>. */
static void
doltgres_index_indexes_arc_drop(StdArcInner *indexes)
{
	std_arc_drop(indexes, sizeof(StdVec), _Alignof(StdVec), doltgres_index_VecIndexed_drop);
}

/*
 * doltgres_index_OldEntries_Drop_drop_closure_0 is OldEntries' drop's work on
 * LISTED: the removal's listing taken out.
 */
static void
doltgres_index_OldEntries_Drop_drop_closure_0(StdVec *listed, void *captures)
{
	int32		slot = *(const int32 *) captures;
	size_t		kept = 0;

	for (size_t i = 0; i < listed->len; i++)
	{
		DoltgresIndexListed *entry = &STD_VEC_AT(listed, DoltgresIndexListed, i);

		if (entry->slot == slot)
		{
			doltgres_index_Listed_drop(entry);
			continue;
		}
		STD_VEC_AT(listed, DoltgresIndexListed, kept) = *entry;
		kept++;
	}
	listed->len = kept;
}

/*
 * doltgres_index_OldEntries_drop is OldEntries' drop: it frees the removal's
 * executor state and memory context, takes it out of the backend's list, then
 * drops its indexes.
 */
static void
doltgres_index_OldEntries_drop(void *record)
{
	DoltgresIndexOldEntries *self = record;

	pg_calls_old_entries_close(self->slot);
	doltgres_index_in_drop(doltgres_index_OldEntries_Drop_drop_closure_0, &self->slot);
	doltgres_index_indexes_arc_drop(self->indexes);
}

/* DOLTGRES_INDEX_OLD_ENTRIES is OLD_ENTRIES, the backend's removals of old entries. */
HostOwnedTable DOLTGRES_INDEX_OLD_ENTRIES = HOST_OWNED_TABLE_NEW;

/* doltgres_index_OldEntries_Owned_table is OldEntries' Owned::table, OLD_ENTRIES. */
static HostOwnedTable *
doltgres_index_OldEntries_Owned_table(void)
{
	return &DOLTGRES_INDEX_OLD_ENTRIES;
}

/* OldEntries' Table, host's owned Table<OldEntries>, with its kind's glue. */
#define HOST_OWNED_SCOPE extern
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_index_OldEntries
#define HOST_OWNED_RS DoltgresIndexOldEntries
#define HOST_OWNED_R DoltgresIndexOldEntries
#define HOST_OWNED_R_DROP doltgres_index_OldEntries_drop
#define HOST_OWNED_KIND 4
#define HOST_OWNED_TABLE_FN doltgres_index_OldEntries_Owned_table
#include "doltgres/host/owned_template.h"

/* OldEntries' Table's with at (). */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_index_OldEntries
#define HOST_OWNED_RS DoltgresIndexOldEntries
#define HOST_OWNED_R DoltgresIndexOldEntries
#define HOST_OWNED_R_DROP doltgres_index_OldEntries_drop
#define HOST_OWNED_KIND 4
#define HOST_OWNED_TABLE_FN doltgres_index_OldEntries_Owned_table
#define HOST_OWNED_WITH_TS Unit
#define HOST_OWNED_WITH_UNIT
#include "doltgres/host/owned_template.h"

/* OldEntries' Table's with at (i32, Arc<Vec<(u32, Described)>>), for opened. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_index_OldEntries
#define HOST_OWNED_RS DoltgresIndexOldEntries
#define HOST_OWNED_R DoltgresIndexOldEntries
#define HOST_OWNED_R_DROP doltgres_index_OldEntries_drop
#define HOST_OWNED_KIND 4
#define HOST_OWNED_TABLE_FN doltgres_index_OldEntries_Owned_table
#define HOST_OWNED_WITH_TS Lent
#define HOST_OWNED_WITH_T doltgres_TupleI32Arc_t
#include "doltgres/host/owned_template.h"

/* doltgres_index_invalidated_closure_0 is invalidated's work on LISTED: each removal of the relation marked stale. */
static void
doltgres_index_invalidated_closure_0(StdVec *listed, void *captures)
{
	uint32		relid = *(const uint32 *) captures;

	for (size_t i = 0; i < listed->len; i++)
	{
		DoltgresIndexListed *entry = &STD_VEC_AT(listed, DoltgresIndexListed, i);
		bool		named = relid == 0 || entry->table == relid;

		for (size_t k = 0; k < entry->indexes.len && !named; k++)
			named = STD_VEC_AT(&entry->indexes, uint32, k) == relid;
		entry->stale |= named;
	}
}

/*
 * doltgres_index_invalidated marks stale each removal of a relation a
 * relcache invalidation names, by its table or one of its indexes, or every
 * removal for an invalidation of every relation (relid 0).
 */
void
doltgres_index_invalidated(uint32 relid)
{
	doltgres_index_in_drop(doltgres_index_invalidated_closure_0, &relid);
}

/* doltgres_index_clear is a list's clear under its lock: every entry dropped. */
static void
doltgres_index_clear(StdVec *kept, void *captures)
{
	kept->len = 0;
}

/* doltgres_index_ended_closure_0 is ended's work on LISTED: each removal's handle copied out, a Vec<Handle>. */
static void
doltgres_index_ended_closure_0(StdVec *listed, void *captures)
{
	StdVec	   *handles = captures;

	for (size_t i = 0; i < listed->len; i++)
		std_vec_push_Checked(handles, &STD_VEC_AT(listed, DoltgresIndexListed, i).handle, sizeof(HostOwnedHandle),
							 _Alignof(HostOwnedHandle));
}

/* doltgres_index_ended_frame_t is ended's state across its pad: the handles it ends. */
typedef struct doltgres_index_ended_frame_t
{
	StdVec		handles;
	HostIoIo   *io;
	HostError  *error;
	bool		ok;
} doltgres_index_ended_frame_t;

/* doltgres_index_ended_pad frees ended's handles. */
static void
doltgres_index_ended_pad(void *frame_)
{
	doltgres_index_ended_frame_t *frame = frame_;

	std_vec_free_Checked(&frame->handles, sizeof(HostOwnedHandle), _Alignof(HostOwnedHandle));
}

/* doltgres_index_ended_body clears UNCHANGED, then ends each listed removal. */
static void
doltgres_index_ended_body(void *frame_)
{
	doltgres_index_ended_frame_t *frame = frame_;

	doltgres_index_unchanged_locked(frame->io, doltgres_index_clear, NULL);
	doltgres_index_listed_locked(frame->io, doltgres_index_ended_closure_0, &frame->handles);
	for (size_t i = 0; i < frame->handles.len; i++)
		if (!host_owned_TableDoltgresIndexOldEntries_end(&DOLTGRES_INDEX_OLD_ENTRIES, frame->io,
														 STD_VEC_AT(&frame->handles, HostOwnedHandle, i),
														 frame->error))
			return;
	frame->ok = true;
}

/*
 * doltgres_index_ended ends every removal of old entries the backend holds, at
 * its top-level transaction's commit or prepare.
 */
bool
doltgres_index_ended(HostIoIo *io, HostError *error)
{
	doltgres_index_ended_frame_t frame = {.io = io, .error = error};

	std_vec_new(&frame.handles, sizeof(HostOwnedHandle), _Alignof(HostOwnedHandle));
	std_landing(&frame, doltgres_index_ended_body, doltgres_index_ended_pad);
	doltgres_index_ended_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_index_listed_found_t is listed's captures: the table it looks for,
 * and the handle and staleness of its removal when found.
 */
typedef struct doltgres_index_listed_found_t
{
	uint32		table;
	bool		found;
	HostOwnedHandle handle;
	bool		stale;
} doltgres_index_listed_found_t;

/* doltgres_index_listed_closure_0 is listed's work on LISTED: the table's removal's handle and staleness. */
static void
doltgres_index_listed_closure_0(StdVec *listed, void *captures)
{
	doltgres_index_listed_found_t *found = captures;

	for (size_t i = 0; i < listed->len && !found->found; i++)
	{
		const DoltgresIndexListed *entry = &STD_VEC_AT(listed, DoltgresIndexListed, i);

		if (entry->table == found->table)
		{
			found->found = true;
			found->handle = entry->handle;
			found->stale = entry->stale;
		}
	}
}

/*
 * doltgres_index_listed answers the handle of a table's removal of old entries
 * the backend holds, or none when it holds none or an invalidation made it
 * stale, which it ends.
 */
bool
doltgres_index_listed(HostIoIo *io, const PgRel *rel, bool *has_result, HostOwnedHandle *result, HostError *error)
{
	doltgres_index_listed_found_t found = {.table = pg_calls_rel_id(rel)};

	doltgres_index_listed_locked(io, doltgres_index_listed_closure_0, &found);
	*has_result = found.found && !found.stale;
	if (*has_result)
		*result = found.handle;
	if (found.found && found.stale)
		return host_owned_TableDoltgresIndexOldEntries_end(&DOLTGRES_INDEX_OLD_ENTRIES, io, found.handle, error);
	return true;
}

/*
 * doltgres_index_removal_frame_t is removal's state across its pad: the
 * descriptions it makes, their object ids, and their Arc.
 */
typedef struct doltgres_index_removal_frame_t
{
	bool		indexes_live;
	StdVec		indexes;
	bool		oids_live;
	StdVec		oids;
	StdArcInner *arc;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const PgRel *rel;
	const access_TupleUsizeOrderKeyType_t *table_key;
	size_t		table_key_len;
	const TxnBaseKey *recorded;
	HostOwnedHandle *result;
	PgPgError  *error;
	bool		ok;
} doltgres_index_removal_frame_t;

/* doltgres_index_removal_pad frees what removal holds. */
static void
doltgres_index_removal_pad(void *frame_)
{
	doltgres_index_removal_frame_t *frame = frame_;

	if (frame->indexes_live)
		doltgres_index_VecIndexed_drop(&frame->indexes);
	if (frame->oids_live)
		std_vec_free_Checked(&frame->oids, DOLTGRES_INDEX_U32);
	if (frame->arc != NULL)
		doltgres_index_indexes_arc_drop(frame->arc);
	frame->arc = NULL;
	frame->indexes_live = frame->oids_live = false;
}

/*
 * doltgres_index_removal_closure_1 is removal's fill of its record: the
 * descriptions' Arc, in place of the empty one.
 */
static bool
doltgres_index_removal_closure_1(void *captures, HostIoIo *io, DoltgresIndexOldEntries *removal, HostError *error)
{
	doltgres_index_removal_frame_t *frame = captures;

	doltgres_index_indexes_arc_drop(removal->indexes);
	removal->indexes = frame->arc;
	frame->arc = NULL;
	return true;
}

/* doltgres_index_removal_push is removal's push of its listing onto LISTED. */
static void
doltgres_index_removal_push(StdVec *listed, void *captures)
{
	std_vec_push_Checked(listed, captures, DOLTGRES_INDEX_LISTED_ELEM);
}

/* doltgres_index_removal_body opens the removal, records it, describes its indexes, and lists it. */
static void
doltgres_index_removal_body(void *frame_)
{
	doltgres_index_removal_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	DoltgresIndexOldEntries unfilled;
	StdVec		empty;
	HostError	failed;
	int32		slot;
	int32		count;
	DoltgresIndexListed listing;

	if (!pg_calls_old_entries_open(raise, catalog, frame->rel, &slot, frame->error))
		return;
	std_vec_new(&empty, DOLTGRES_INDEX_INDEXED);
	unfilled.slot = slot;
	unfilled.indexes = std_arc_new(&empty, sizeof(StdVec), _Alignof(StdVec));
	if (!host_owned_TableDoltgresIndexOldEntries_make_in_transaction(&DOLTGRES_INDEX_OLD_ENTRIES, frame->io, unfilled,
																	 frame->result, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	std_vec_new(&frame->indexes, DOLTGRES_INDEX_INDEXED);
	frame->indexes_live = true;
	if (!pg_calls_old_entries_opened(raise, catalog, slot, frame->error))
		return;
	count = pg_calls_old_entries_count(slot);
	for (int32 at = 0; at < count; at++)
	{
		const PgRel *index = pg_calls_old_entries_index(frame->rel, slot, at);
		doltgres_TupleU32Described_t indexed;

		if (!doltgres_index_describe(raise, catalog, index, frame->table_key, frame->table_key_len, frame->recorded,
									 &indexed.f1, frame->error))
		{
			pg_calls_old_entries_unopened(slot);
			return;
		}
		indexed.f0 = pg_calls_rel_id(index);
		std_vec_push_Checked(&frame->indexes, &indexed, DOLTGRES_INDEX_INDEXED);
	}
	pg_calls_old_entries_unopened(slot);
	std_vec_with_capacity_Checked(&frame->oids, frame->indexes.len, DOLTGRES_INDEX_U32);
	frame->oids_live = true;
	for (size_t i = 0; i < frame->indexes.len; i++)
		std_vec_push_Checked(&frame->oids, &STD_VEC_AT(&frame->indexes, doltgres_TupleU32Described_t, i).f0,
							 DOLTGRES_INDEX_U32);
	frame->arc = std_arc_new(&frame->indexes, sizeof(StdVec), _Alignof(StdVec));
	frame->indexes_live = false;
	if (!host_owned_TableDoltgresIndexOldEntries_withUnit(&DOLTGRES_INDEX_OLD_ENTRIES, frame->io, *frame->result,
														  doltgres_index_removal_closure_1, frame, NULL, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	listing.table = pg_calls_rel_id(frame->rel);
	listing.indexes = frame->oids;
	listing.slot = slot;
	listing.handle = *frame->result;
	listing.stale = false;
	doltgres_index_listed_locked(frame->io, doltgres_index_removal_push, &listing);
	frame->oids_live = false;
	frame->ok = true;
}

/*
 * doltgres_index_removal makes a table's removal of old entries under the
 * top-level transaction's resource owner and answers its handle, with each
 * of its indexes the fork stores described under the table's keyed layout.
 */
bool
doltgres_index_removal(const PgCatalogCaps *caps, HostIoIo *io, const PgRel *rel,
					   const access_TupleUsizeOrderKeyType_t *table_key, size_t table_key_len,
					   const TxnBaseKey *recorded, HostOwnedHandle *result, PgPgError *error)
{
	doltgres_index_removal_frame_t frame = {.caps = caps, .io = io, .rel = rel, .table_key = table_key,
	.table_key_len = table_key_len, .recorded = recorded, .result = result, .error = error};

	std_landing(&frame, doltgres_index_removal_body, doltgres_index_removal_pad);
	doltgres_index_removal_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_index_read answers a row's values of a removal's index that keeps
 * no records, none where its predicate leaves the row out, formed by
 * Postgres from the row a slot holds (FormIndexDatum).
 */
static bool
doltgres_index_read(const PgCatalogCaps *caps, const PgRel *rel, int32 slot, int32 at,
					const DoltgresIndexDescribed *described, PgSlot *row, bool *has_result, StdVec *result,
					PgPgError *error)
{
	bool		formed;

	*has_result = false;
	if (!pg_calls_old_entries_form(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), slot, at, row,
								   &formed, error))
		return false;
	if (!formed)
		return true;
	if (!doltgres_index_Described_values(described, caps, pg_calls_old_entries_values(rel, slot),
										 pg_calls_old_entries_nulls(rel, slot), result, error))
		return false;
	*has_result = true;
	return true;
}

/* doltgres_index_opened_closure_0 is opened's lend: the removal's slot and a clone of its descriptions' Arc. */
static bool
doltgres_index_opened_closure_0(void *captures, HostIoIo *io, DoltgresIndexOldEntries *removal,
								doltgres_TupleI32Arc_t *result, HostError *error)
{
	result->f0 = removal->slot;
	result->f1 = std_arc_clone(removal->indexes);
	return true;
}

/* doltgres_index_opened_frame_t is opened's state across its pad: the descriptions' Arc it lends work. */
typedef struct doltgres_index_opened_frame_t
{
	StdArcInner *indexes;
	int32		slot;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	HostOwnedHandle handle;
	DoltgresIndexWork work;
	void	   *captures;
	PgPgError  *error;
	bool		ok;
} doltgres_index_opened_frame_t;

/* doltgres_index_opened_pad drops the descriptions' Arc. */
static void
doltgres_index_opened_pad(void *frame_)
{
	doltgres_index_opened_frame_t *frame = frame_;

	if (frame->indexes != NULL)
		doltgres_index_indexes_arc_drop(frame->indexes);
	frame->indexes = NULL;
}

/* doltgres_index_opened_body lends the removal's indexes, opens them, and runs the work over them. */
static void
doltgres_index_opened_body(void *frame_)
{
	doltgres_index_opened_frame_t *frame = frame_;
	doltgres_TupleI32Arc_t lent;
	HostError	failed;
	const StdVec *indexes;

	if (!host_owned_TableDoltgresIndexOldEntries_withLent(&DOLTGRES_INDEX_OLD_ENTRIES, frame->io, frame->handle,
														  doltgres_index_opened_closure_0, NULL, NULL, &lent, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->slot = lent.f0;
	frame->indexes = lent.f1;
	if (!pg_calls_old_entries_opened(pg_CatalogCaps_may_raise(frame->caps), pg_CatalogCaps_catalog(frame->caps),
									 frame->slot, frame->error))
		return;
	indexes = std_arc_Deref_deref(frame->indexes, _Alignof(StdVec));
	frame->ok = frame->work(frame->captures, frame->caps, frame->io, frame->slot, indexes->ptr, indexes->len,
							frame->error);
	pg_calls_old_entries_unopened(frame->slot);
}

/*
 * doltgres_index_opened runs work over a removal's indexes while they are
 * open, with the removal's slot and each index's object id and description,
 * cloning the record's Arc of its descriptions, since work runs outside the
 * record's lend.
 */
bool
doltgres_index_opened(const PgCatalogCaps *caps, HostIoIo *io, HostOwnedHandle handle, DoltgresIndexWork work,
					  void *captures, PgPgError *error)
{
	doltgres_index_opened_frame_t frame = {.caps = caps, .io = io, .handle = handle, .work = work,
	.captures = captures, .error = error};

	std_landing(&frame, doltgres_index_opened_body, doltgres_index_opened_pad);
	doltgres_index_opened_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_index_removing_frame_t is removing's state across its pad: an
 * index's old values and its new values, and the old entry it hands remove.
 */
typedef struct doltgres_index_removing_frame_t
{
	bool		old_live;
	StdVec		old_values;
	bool		new_live;
	StdVec		new_values;
	DoltgresIndexOld entry;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const PgRel *rel;
	int32		slot;
	const doltgres_TupleU32Described_t *indexes;
	size_t		indexes_len;
	const TxnMemtableVersionWriter *writer;
	DoltgresIndexRemoving *rows;
	DoltgresIndexRemove remove;
	void	   *remove_captures;
	bool	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_index_removing_frame_t;

/* doltgres_index_removing_drop_values frees an index's old and new values while live. */
static void
doltgres_index_removing_drop_values(doltgres_index_removing_frame_t *frame)
{
	if (frame->old_live)
		doltgres_index_values_drop(&frame->old_values);
	if (frame->new_live)
		doltgres_index_values_drop(&frame->new_values);
	frame->old_live = frame->new_live = false;
}

/* doltgres_index_removing_pad frees what removing holds. */
static void
doltgres_index_removing_pad(void *frame_)
{
	doltgres_index_removing_frame_t *frame = frame_;

	doltgres_index_removing_drop_values(frame);
}

/* doltgres_index_values_eq reports whether two rows' values, as values reads them, are equal. */
static bool
doltgres_index_values_eq(const StdVec *a, const StdVec *b)
{
	if (a->len != b->len)
		return false;
	for (size_t i = 0; i < a->len; i++)
	{
		const access_OptionVecU8_t *x = &STD_VEC_AT(a, access_OptionVecU8_t, i);
		const access_OptionVecU8_t *y = &STD_VEC_AT(b, access_OptionVecU8_t, i);

		if (x->has != y->has ||
			(x->has && (x->bytes.len != y->bytes.len || memcmp(x->bytes.ptr, y->bytes.ptr, x->bytes.len) != 0)))
			return false;
	}
	return true;
}

/* doltgres_index_removing_kept is removing's push of an entry the update kept onto UNCHANGED. */
static void
doltgres_index_removing_kept(StdVec *kept, void *captures)
{
	std_vec_push_Checked(kept, captures, DOLTGRES_INDEX_KEPT_ELEM);
}

/* doltgres_index_removing_body removes each index's old entry when it changes and lists each entry an update kept. */
static void
doltgres_index_removing_body(void *frame_)
{
	doltgres_index_removing_frame_t *frame = frame_;
	DoltgresIndexRemoving *rows = frame->rows;
	StdBytes	old_key = rows->old_key;
	bool		changed = pg_calls_old_entries_others(frame->slot);
	int32		at = 0;

	for (size_t k = 0;; k++)
	{
		int32		taken = at;
		const doltgres_TupleU32Described_t *indexed;
		const DoltgresIndexDescribed *described;
		bool		same_key;
		bool		kept;
		bool		same = false;

		at = std_forward_i32_Checked(at, 1);
		if (k >= frame->indexes_len)
			break;
		indexed = &frame->indexes[k];
		described = &indexed->f1;
		same_key = rows->has_new && rows->new_key.len == old_key.len &&
			memcmp(rows->new_key.ptr, old_key.ptr, old_key.len) == 0;
		if (doltgres_index_Described_keyed(described))
		{
			changed |= rows->has_new && !same_key;
			continue;
		}
		kept = !doltgres_index_Described_keyless(described) && same_key;
		if (doltgres_index_Described_records(described))
		{
			if (rows->has_new && kept &&
				!pg_calls_old_entries_unchanged(pg_CatalogCaps_may_raise(frame->caps), frame->slot, taken,
												rows->old_slot, rows->new_slot, &same, frame->error))
				return;
		}
		else
		{
			if (!doltgres_index_read(frame->caps, frame->rel, frame->slot, taken, described, rows->old_slot,
									 &frame->old_live, &frame->old_values, frame->error))
				return;
			if (rows->has_new && kept)
			{
				if (!doltgres_index_read(frame->caps, frame->rel, frame->slot, taken, described, rows->new_slot,
										 &frame->new_live, &frame->new_values, frame->error))
					return;
				same = frame->new_live == frame->old_live &&
					(!frame->old_live || doltgres_index_values_eq(&frame->new_values, &frame->old_values));
				if (frame->new_live)
					doltgres_index_values_drop(&frame->new_values);
				frame->new_live = false;
			}
		}
		if (same)
		{
			if (rows->has_new)
			{
				doltgres_TupleU32U64U64U32_t entry = {indexed->f0, rows->new_tuple, frame->writer->xid,
				frame->writer->command};

				doltgres_index_unchanged_locked(frame->io, doltgres_index_removing_kept, &entry);
			}
			doltgres_index_removing_drop_values(frame);
			continue;
		}
		changed = true;
		if (doltgres_index_Described_records(described))
		{
			frame->entry.tag = DOLTGRES_INDEX_OLD_RECORDED;
			doltgres_index_removing_drop_values(frame);
		}
		else if (frame->old_live)
		{
			AccessIndexEntryKey key;
			AccessKeySortKeys sorter = {&ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS, frame->caps};

			if (!doltgres_index_Described_key(described, sorter, frame->old_values.ptr, frame->old_values.len,
											  old_key.ptr, old_key.len, &key, frame->error))
				return;
			std_vec_free_Checked(&key.cut, sizeof(bool), _Alignof(bool));
			frame->entry.tag = DOLTGRES_INDEX_OLD_KEY;
			frame->entry.key = key.key;
			doltgres_index_removing_drop_values(frame);
		}
		else
			continue;
		if (!frame->remove(frame->remove_captures, frame->caps, frame->io,
						   pg_calls_old_entries_index(frame->rel, frame->slot, taken), &frame->entry, described,
						   frame->error))
			return;
	}
	*frame->result = changed;
	frame->ok = true;
}

/*
 * doltgres_index_removing is removed's work over a removal's opened indexes:
 * each index's old entry removed when it changes, and each entry an update
 * kept listed for the update's own aminsert.
 */
static bool
doltgres_index_removing(const PgCatalogCaps *caps, HostIoIo *io, const PgRel *rel, int32 slot,
						const doltgres_TupleU32Described_t *indexes, size_t indexes_len,
						const TxnMemtableVersionWriter *writer, DoltgresIndexRemoving *rows, DoltgresIndexRemove remove,
						void *remove_captures, bool *result, PgPgError *error)
{
	doltgres_index_removing_frame_t frame = {.caps = caps, .io = io, .rel = rel, .slot = slot, .indexes = indexes,
		.indexes_len = indexes_len, .writer = writer, .rows = rows, .remove = remove,
	.remove_captures = remove_captures, .result = result, .error = error};

	std_landing(&frame, doltgres_index_removing_body, doltgres_index_removing_pad);
	doltgres_index_removing_pad(&frame);
	return frame.ok;
}

/* doltgres_index_removed_t is removed's captures for its work: removing's arguments beside the removal's. */
typedef struct doltgres_index_removed_t
{
	const PgRel *rel;
	const TxnMemtableVersionWriter *writer;
	DoltgresIndexRemoving *rows;
	DoltgresIndexRemove remove;
	void	   *remove_captures;
	bool	   *result;
} doltgres_index_removed_t;

/* doltgres_index_removed_closure_0 is removed's work over the removal's opened indexes: removing's. */
static bool
doltgres_index_removed_closure_0(void *captures, const PgCatalogCaps *caps, HostIoIo *io, int32 slot,
								 const doltgres_TupleU32Described_t *indexes, size_t indexes_len, PgPgError *error)
{
	doltgres_index_removed_t *removed = captures;

	return doltgres_index_removing(caps, io, removed->rel, slot, indexes, indexes_len, removed->writer, removed->rows,
								   removed->remove, removed->remove_captures, removed->result, error);
}

/*
 * doltgres_index_removed is the table AM's removal of old entries through a
 * removal's handle: for each index of the table the fork stores, the old
 * row's entry is removed by remove when it changes, named by its key formed
 * from the old row's columns or by the row's record; an update's new row's
 * entry is taken as the same, which its aminsert then skips, when nothing it
 * reads changed. It answers whether an index must take the new row.
 */
bool
doltgres_index_removed(const PgCatalogCaps *caps, HostIoIo *io, const PgRel *rel, HostOwnedHandle handle,
					   const TxnMemtableVersionWriter *writer, DoltgresIndexRemoving *rows, DoltgresIndexRemove remove,
					   void *remove_captures, bool *result, PgPgError *error)
{
	doltgres_index_removed_t removed = {rel, writer, rows, remove, remove_captures, result};

	doltgres_index_unchanged_locked(io, doltgres_index_clear, NULL);
	return doltgres_index_opened(caps, io, handle, doltgres_index_removed_closure_0, &removed, error);
}
#endif
