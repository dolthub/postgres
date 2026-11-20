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
 * A fold's re-form of a keyed table's rows to the end of its pending
 * transforms (doltgres' reform.rs): each row read through its composed
 * transform into a slot, every value whole, and placed by the catalog's
 * layout as a fresh insert places it.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/access/slot.h"
#include "doltgres/access/transform.h"
#include "doltgres/doltgres/entries.h"
#include "doltgres/doltgres/layout.h"
#include "doltgres/doltgres/reform.h"
#include "doltgres/doltgres/scan.h"
#include "doltgres/encoding/tuple.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/blob.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/blobs.h"
#include "doltgres/txn/visible.h"
#include "pg_calls.h"

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "reform.rs"

/* The element layouts of re-form's vectors. */
#define DOLTGRES_REFORM_BYTES 1, 1
#define DOLTGRES_REFORM_COMPOSED sizeof(DoltgresReformComposed), _Alignof(DoltgresReformComposed)
#define DOLTGRES_REFORM_TRANSFORM sizeof(AccessTransformTransform), _Alignof(AccessTransformTransform)
#define DOLTGRES_REFORM_KEPT sizeof(DoltgresReformKept), _Alignof(DoltgresReformKept)
#define DOLTGRES_REFORM_MOVED sizeof(doltgres_TupleUsizeU32_t), _Alignof(doltgres_TupleUsizeU32_t)
#define DOLTGRES_REFORM_U32 sizeof(uint32), _Alignof(uint32)
#define DOLTGRES_REFORM_SET sizeof(AccessHeaptupleSet), _Alignof(AccessHeaptupleSet)
#define DOLTGRES_REFORM_ADDRESS sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t)
#define DOLTGRES_REFORM_COLUMN sizeof(AccessHeaptupleColumn), _Alignof(AccessHeaptupleColumn)

/*
 * DoltgresReformKept is an entry of old_values' map: a value's blob's
 * address and its value id, the map a Vec<Kept> sorted by address, as the
 * port's internals may hold it.
 */
typedef struct DoltgresReformKept
{
	uint8		address[32];
	uint32		id;
} DoltgresReformKept;

/* doltgres_reform_moved_lt is the fn item (usize, u32)'s PartialOrd::lt's FnMut shim, which sort_unstable passes. */
static bool
doltgres_reform_moved_lt(void *captures, const void *a, const void *b)
{
	const doltgres_TupleUsizeU32_t *x = a;
	const doltgres_TupleUsizeU32_t *y = b;

	return x->f0 < y->f0 || (x->f0 == y->f0 && x->f1 < y->f1);
}

/* <[(usize, u32)]>::sort_unstable, which reformed_row sorts its moved values by. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS TupleUsizeU32
#define STD_SORT_T doltgres_TupleUsizeU32_t
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT doltgres_reform_moved_lt
#include "doltgres/std/sort_template.h"

/* doltgres_reform_internal returns an internal error of a re-form. */
static PgPgError
doltgres_reform_internal(const char *message)
{
	StdString text;

	std_string_from_Checked(&text, message, strlen(message));
	return pg_PgError_fork((const uint8 *) "XX000", &text);
}

/* doltgres_reform_Composed_drop is a kept composition's drop glue, an element of Catalog's composed. */
static void
doltgres_reform_Composed_drop(void *composed)
{
	std_vec_free_Checked(&((DoltgresReformComposed *) composed)->bytes, DOLTGRES_REFORM_BYTES);
}

/*
 * doltgres_reform_Catalog_new returns the re-form of a table's rows through
 * its catalog's layout, its key's columns given. Its caller holds a lock on
 * the relation from before its fold reads the chain until the fold installs,
 * so no DDL's transform joins the chain while the catalog's layout is read.
 */
void
doltgres_reform_Catalog_new(const PgRel *rel, const access_TupleUsizeOrderKeyType_t *key, size_t key_len,
							DoltgresReformCatalog *result)
{
	result->relation = pg_calls_rel_id(rel);
	access_slot_keyed(key, key_len, &result->keyed);
	std_vec_new(&result->composed, DOLTGRES_REFORM_COMPOSED);
}

/* doltgres_reform_Catalog_drop is Catalog's drop glue: its keyed bytes, then its kept compositions. */
void
doltgres_reform_Catalog_drop(DoltgresReformCatalog *self)
{
	std_vec_free_Checked(&self->keyed, DOLTGRES_REFORM_BYTES);
	std_ptr_drop_in_place_slice(self->composed.ptr, self->composed.len, sizeof(DoltgresReformComposed),
								doltgres_reform_Composed_drop);
	std_vec_free_Checked(&self->composed, DOLTGRES_REFORM_COMPOSED);
}

/* doltgres_reform_composed_at returns where a count's composition is kept in Catalog's sorted compositions, or goes. */
static size_t
doltgres_reform_composed_at(const DoltgresReformCatalog *self, size_t count, bool *found)
{
	size_t		low = 0;
	size_t		high = self->composed.len;

	*found = false;
	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		size_t		at = STD_VEC_AT(&self->composed, DoltgresReformComposed, mid).count;

		if (at == count)
		{
			*found = true;
			return mid;
		}
		if (at < count)
			low = mid + 1;
		else
			high = mid;
	}
	return low;
}

/* doltgres_reform_Transform_drop is Transform's drop glue, an element of transform's decoded chain. */
static void
doltgres_reform_Transform_drop(void *transform)
{
	access_transform_Transform_drop(transform);
}

/* doltgres_reform_transform_frame_t is transform's state across its pad: the chain decoded so far, while live. */
typedef struct doltgres_reform_transform_frame_t
{
	StdVec		decoded;
	bool		decoded_live;
	DoltgresReformCatalog *self;
	const TxnVisibleLive *transforms;
	size_t		transforms_len;
	StdVec	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_reform_transform_frame_t;

/* doltgres_reform_transform_pad frees what transform holds. */
static void
doltgres_reform_transform_pad(void *frame_)
{
	doltgres_reform_transform_frame_t *frame = frame_;

	if (frame->decoded_live)
	{
		std_ptr_drop_in_place_slice(frame->decoded.ptr, frame->decoded.len, sizeof(AccessTransformTransform),
									doltgres_reform_Transform_drop);
		std_vec_free_Checked(&frame->decoded, DOLTGRES_REFORM_TRANSFORM);
	}
	frame->decoded_live = false;
}

/* doltgres_reform_transform_body decodes the chain's transforms, composes them, and keeps the encoded composition. */
static void
doltgres_reform_transform_body(void *frame_)
{
	doltgres_reform_transform_frame_t *frame = frame_;
	AccessTransformTransform composed;
	DoltgresReformComposed kept;
	bool		has;
	bool		found;
	size_t		at;

	std_vec_with_capacity_Checked(&frame->decoded, frame->transforms_len, DOLTGRES_REFORM_TRANSFORM);
	frame->decoded_live = true;
	for (size_t i = 0; i < frame->transforms_len; i++)
	{
		const StdVec *bytes = &frame->transforms[i].f0.transform;
		AccessTransformTransform transform;

		if (!access_transform_Transform_decode(bytes->ptr, bytes->len, &transform, frame->error))
			return;
		std_vec_push_Checked(&frame->decoded, &transform, DOLTGRES_REFORM_TRANSFORM);
	}
	if (!access_transform_compose(frame->decoded.ptr, frame->decoded.len, &has, &composed, frame->error))
		return;
	if (has)
	{
		access_transform_Transform_encode(&composed, &kept.bytes);
		access_transform_Transform_drop(&composed);
	}
	else
		std_vec_new(&kept.bytes, DOLTGRES_REFORM_BYTES);
	kept.count = frame->transforms_len;
	std_vec_clone(frame->result, &kept.bytes, DOLTGRES_REFORM_BYTES);
	at = doltgres_reform_composed_at(frame->self, kept.count, &found);
	std_vec_insert(&frame->self->composed, at, &kept, DOLTGRES_REFORM_COMPOSED, RUST_FILE, 59, 23);
	frame->ok = true;
}

/*
 * doltgres_reform_Catalog_transform returns the encoded composition of a
 * chain's transforms, kept by their count for the next row; it clones the
 * kept bytes, since the Catalog keeps its own.
 */
static bool
doltgres_reform_Catalog_transform(DoltgresReformCatalog *self, const TxnVisibleLive *transforms, size_t transforms_len,
								  StdVec *result, PgPgError *error)
{
	doltgres_reform_transform_frame_t frame = {.self = self, .transforms = transforms,
	.transforms_len = transforms_len, .result = result, .error = error};
	bool		found;
	size_t		at = doltgres_reform_composed_at(self, transforms_len, &found);

	if (found)
	{
		std_vec_clone(result, &STD_VEC_AT(&self->composed, DoltgresReformComposed, at).bytes, DOLTGRES_REFORM_BYTES);
		return true;
	}
	std_landing(&frame, doltgres_reform_transform_body, doltgres_reform_transform_pad);
	doltgres_reform_transform_pad(&frame);
	return frame.ok;
}

/* doltgres_reform_reformed_frame_t is reformed's state across its pad: the chain's composition, while live. */
typedef struct doltgres_reform_reformed_frame_t
{
	StdVec		transform;
	bool		transform_live;
	DoltgresReformCatalog *self;
	HostIoIo   *io;
	const uint8 *key;
	size_t		key_len;
	ChunksTrailerStamp stamp;
	const uint8 *tuple;
	size_t		tuple_len;
	const uint32 *values;
	size_t		values_len;
	const TxnVisibleLive *transforms;
	size_t		transforms_len;
	TxnFoldReformed *result;
	HostError  *error;
	bool		ok;
} doltgres_reform_reformed_frame_t;

/* doltgres_reform_reformed_pad frees what reformed holds. */
static void
doltgres_reform_reformed_pad(void *frame_)
{
	doltgres_reform_reformed_frame_t *frame = frame_;

	if (frame->transform_live)
		std_vec_free_Checked(&frame->transform, DOLTGRES_REFORM_BYTES);
	frame->transform_live = false;
}

/* doltgres_reform_reformed_body re-forms the row through host's reformed door with its chain's composition. */
static void
doltgres_reform_reformed_body(void *frame_)
{
	doltgres_reform_reformed_frame_t *frame = frame_;
	HostReformRow row;
	HostReformedRow reformed;
	PgPgError	failed;

	if (!doltgres_reform_Catalog_transform(frame->self, frame->transforms, frame->transforms_len, &frame->transform,
										   &failed))
	{
		*frame->error = pghost_held(&failed);
		return;
	}
	frame->transform_live = true;
	row.relation = frame->self->relation;
	row.keyed = frame->self->keyed.ptr;
	row.keyed_len = frame->self->keyed.len;
	row.key = frame->key;
	row.key_len = frame->key_len;
	row.stamp.f0 = frame->stamp.txn;
	row.stamp.f1 = frame->stamp.tuple;
	row.tuple = frame->tuple;
	row.tuple_len = frame->tuple_len;
	row.values = frame->values;
	row.values_len = frame->values_len;
	row.transform = frame->transform.ptr;
	row.transform_len = frame->transform.len;
	if (!host_reformed(frame->io, &row, &reformed, frame->error))
		return;
	std_vec_with_capacity_Checked(&frame->result->addresses, reformed.addresses.len, DOLTGRES_REFORM_ADDRESS);
	for (size_t i = 0; i < reformed.addresses.len; i++)
	{
		chunks_TupleU32Locator_t address = {STD_VEC_AT(&reformed.addresses, uint32, i)};

		std_vec_push_Checked(&frame->result->addresses, &address, DOLTGRES_REFORM_ADDRESS);
	}
	std_vec_free_Checked(&reformed.addresses, DOLTGRES_REFORM_U32);
	frame->result->tuple = reformed.tuple;
	frame->result->values = reformed.values;
	frame->result->heap_bytes = reformed.heap_bytes;
	frame->ok = true;
}

/*
 * doltgres_reform_Catalog_Reform_reformed is Catalog's Reform::reformed: a
 * row re-formed through host's reformed door, whose catalog work is
 * reformed_row's, with its chain's composed transform.
 */
static bool
doltgres_reform_Catalog_Reform_reformed(void *self, HostIoIo *io, const uint8 *key, size_t key_len,
										ChunksTrailerStamp stamp, const uint8 *tuple, size_t tuple_len,
										const chunks_TupleU32Locator_t *addresses, size_t addresses_len,
										const uint32 *values, size_t values_len, const TxnVisibleLive *transforms,
										size_t transforms_len, TxnFoldReformed *result, HostError *error)
{
	doltgres_reform_reformed_frame_t frame = {.self = self, .io = io, .key = key, .key_len = key_len,
		.stamp = stamp, .tuple = tuple, .tuple_len = tuple_len, .values = values, .values_len = values_len,
	.transforms = transforms, .transforms_len = transforms_len, .result = result, .error = error};

	std_landing(&frame, doltgres_reform_reformed_body, doltgres_reform_reformed_pad);
	doltgres_reform_reformed_pad(&frame);
	return frame.ok;
}

/* DOLTGRES_REFORM_CATALOG_REFORM_OPS is Catalog's Reform. */
const TxnFoldReformOps DOLTGRES_REFORM_CATALOG_REFORM_OPS = {doltgres_reform_Catalog_Reform_reformed};

/* doltgres_reform_kept_at returns where an address is in old_values' sorted map, or goes. */
static size_t
doltgres_reform_kept_at(const StdVec *kept, const uint8 *address, bool *found)
{
	size_t		low = 0;
	size_t		high = kept->len;

	*found = false;
	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		int			cmp = memcmp(STD_VEC_AT(kept, DoltgresReformKept, mid).address, address, 32);

		if (cmp == 0)
		{
			*found = true;
			return mid;
		}
		if (cmp < 0)
			low = mid + 1;
		else
			high = mid;
	}
	return low;
}

/*
 * doltgres_reform_old_values_closure_0 returns the error of a stored row
 * whose values out of line don't match its value ids.
 */
static PgPgError
doltgres_reform_old_values_closure_0(void)
{
	StdString text;
	static const char message[] = "a stored row's values out of line don't match its value ids";

	std_string_from_Checked(&text, message, sizeof(message) - 1);
	return pg_PgError_fork((const uint8 *) "XX001", &text);
}

/*
 * doltgres_reform_old_values returns a stored row's values out of line by
 * their blobs' addresses, each with its value id, in its fields' order,
 * refusing a row with an overflow chunk, whose values a re-form doesn't read
 * yet.
 */
static bool
doltgres_reform_old_values(const uint8 *tuple, size_t tuple_len, const uint32 *ids, size_t ids_len, StdVec *result,
						   PgPgError *error)
{
	EncodingTupleTuple parsed;
	EncodingTupleFields fields;
	EncodingTupleField field;
	StdBytes	overflow_node;
	size_t		overflow;
	size_t		count = 0;

	if (!encoding_tuple_Tuple_parse(tuple, tuple_len, &parsed))
	{
		*error = doltgres_reform_old_values_closure_0();
		return false;
	}
	if (encoding_tuple_Tuple_overflow(&parsed, &overflow_node, &overflow))
	{
		*error = doltgres_reform_internal("a re-form of a row with an overflow chunk isn't built");
		return false;
	}
	fields = encoding_tuple_Tuple_fields(&parsed);
	while (encoding_tuple_Fields_Iterator_next(&fields, &field))
	{
		if (field.tag != ENCODING_TUPLE_FIELD_EXTERNAL)
			continue;
		if (field.u.external.address.len != 32)
		{
			*error = doltgres_reform_old_values_closure_0();
			return false;
		}
		count++;
	}
	if (count != ids_len)
	{
		*error = doltgres_reform_old_values_closure_0();
		return false;
	}
	std_vec_new(result, DOLTGRES_REFORM_KEPT);
	fields = encoding_tuple_Tuple_fields(&parsed);
	count = 0;
	while (encoding_tuple_Fields_Iterator_next(&fields, &field))
	{
		DoltgresReformKept kept;
		bool		found;
		size_t		at;

		if (field.tag != ENCODING_TUPLE_FIELD_EXTERNAL)
			continue;
		memcpy(kept.address, field.u.external.address.ptr, 32);
		kept.id = ids[count++];
		at = doltgres_reform_kept_at(result, kept.address, &found);
		if (found)
			STD_VEC_AT(result, DoltgresReformKept, at).id = kept.id;
		else
			std_vec_insert(result, at, &kept, DOLTGRES_REFORM_KEPT, RUST_FILE, 170, 59);
	}
	return true;
}

/*
 * doltgres_reform_reformed_row_frame_t is reformed_row's state across its
 * pad: the table opened, its slot, the stored row, the row's columns and
 * held values and the values they set, its layout, its old values out of
 * line, its placing, the values it moved, a blob's cut, and the formed row,
 * each while live.
 */
typedef struct doltgres_reform_reformed_row_frame_t
{
	PgRefRel	opened;
	bool		opened_live;
	PgOwnedSlot owned;
	bool		owned_live;
	DoltgresScanRow stored;
	bool		stored_live;
	StdVec		columns;
	bool		columns_live;
	StdVec		held;
	bool		held_live;
	StdVec		set;
	bool		set_live;
	AccessHeaptupleRowLayout layout;
	bool		layout_live;
	StdVec		kept;
	bool		kept_live;
	AccessHeaptuplePlacing placing;
	bool		placing_live;
	StdVec		moved;
	bool		moved_live;
	TxnBlobsCut cut;
	bool		cut_live;
	AccessHeaptupleFormed formed;
	bool		formed_live;
	const PgCatalogCaps *caps;
	const HostReformRow *row;
	HostReformedRow *result;
	PgPgError  *error;
	bool		ok;
} doltgres_reform_reformed_row_frame_t;

/* doltgres_reform_reformed_row_pad frees what reformed_row holds, in the reverse of its making. */
static void
doltgres_reform_reformed_row_pad(void *frame_)
{
	doltgres_reform_reformed_row_frame_t *frame = frame_;

	if (frame->formed_live)
		access_heaptuple_Formed_drop(&frame->formed);
	if (frame->cut_live)
		txn_blobs_Cut_drop(&frame->cut);
	if (frame->moved_live)
		std_vec_free_Checked(&frame->moved, DOLTGRES_REFORM_MOVED);
	if (frame->placing_live)
		access_heaptuple_Placing_drop(&frame->placing);
	if (frame->kept_live)
		std_vec_free_Checked(&frame->kept, DOLTGRES_REFORM_KEPT);
	if (frame->layout_live)
		access_heaptuple_RowLayout_drop(&frame->layout);
	if (frame->set_live)
		std_vec_free_Checked(&frame->set, DOLTGRES_REFORM_SET);
	if (frame->held_live)
		doltgres_layout_VecHeld_drop(&frame->held);
	if (frame->columns_live)
		std_vec_free_Checked(&frame->columns, DOLTGRES_REFORM_COLUMN);
	if (frame->stored_live)
		doltgres_scan_Row_drop(&frame->stored);
	if (frame->owned_live)
		pg_OwnedSlot_drop(&frame->owned);
	if (frame->opened_live)
		pg_RefRel_drop(&frame->opened);
	frame->formed_live = frame->cut_live = frame->moved_live = frame->placing_live = frame->kept_live = false;
	frame->layout_live = frame->set_live = frame->held_live = frame->columns_live = frame->stored_live = false;
	frame->owned_live = frame->opened_live = false;
}

/* doltgres_reform_stored_of makes the stored row a re-form's row stands for, as a read gives it. */
static void
doltgres_reform_stored_of(const HostReformRow *row, DoltgresScanRow *result)
{
	std_slice_to_vec_Checked(&result->keyed, row->keyed, row->keyed_len, DOLTGRES_REFORM_BYTES);
	result->table = row->relation;
	std_slice_to_vec_Checked(&result->key, row->key, row->key_len, DOLTGRES_REFORM_BYTES);
	std_slice_to_vec_Checked(&result->tuple, row->tuple, row->tuple_len, DOLTGRES_REFORM_BYTES);
	std_vec_new(&result->values, DOLTGRES_REFORM_BYTES);
	for (size_t i = 0; i < row->values_len; i++)
	{
		uint32		id = row->values[i];
		uint8		bytes[4] = {(uint8) id, (uint8) (id >> 8), (uint8) (id >> 16), (uint8) (id >> 24)};

		std_vec_extend_from_slice_Checked(&result->values, bytes, 4, DOLTGRES_REFORM_BYTES);
	}
	std_slice_to_vec_Checked(&result->transform, row->transform, row->transform_len, DOLTGRES_REFORM_BYTES);
	result->stamp.txn = row->stamp.f0;
	result->stamp.tuple = row->stamp.f1;
	result->top = 0;
	result->command = 0;
	result->has_changer = false;
	result->dirty = txn_visible_Dirty_Default_default();
}

/*
 * doltgres_reform_reformed_row_body opens the table, reads the stored row
 * into a slot through its transform, and places its values by the catalog's
 * layout, each value it moves out of line named by the value id the stored
 * row kept for its blob.
 */
static void
doltgres_reform_reformed_row_body(void *frame_)
{
	doltgres_reform_reformed_row_frame_t *frame = frame_;
	const HostReformRow *row = frame->row;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	AccessKeySortKeys sorter = {&ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS, frame->caps};
	AccessKeyNames names = {&DOLTGRES_LAYOUT_BLOB_NAMES_NAMES_OPS, NULL};
	AccessHeaptupleBlob blob;
	const PgRel *rel;
	PgSlot	   *slot;
	StdVec		key;
	int32		toast_target;
	size_t		target;
	bool		has;

	if (!pg_RefRel_opened(raise, catalog, row->relation, &has, &frame->opened, frame->error))
		return;
	if (!has)
	{
		*frame->error = doltgres_reform_internal("a fold's re-form found no table of its storage");
		return;
	}
	frame->opened_live = true;
	rel = pg_RefRel_rel(&frame->opened);
	toast_target = pg_calls_rel_toast_target(rel);
	target = toast_target < 0 ? 0 : (size_t) toast_target;
	if (!pg_calls_make_slot(raise, rel, &frame->owned, frame->error))
		return;
	frame->owned_live = true;
	doltgres_reform_stored_of(row, &frame->stored);
	frame->stored_live = true;
	if (!doltgres_entries_stored(raise, pg_OwnedSlot_slot(&frame->owned), rel, &frame->stored, frame->error))
		return;
	slot = pg_OwnedSlot_slot(&frame->owned);
	if (!pg_calls_slot_getallattrs(raise, slot, frame->error))
		return;
	if (!doltgres_layout_slot_row(raise, catalog, rel, slot, &frame->columns, &frame->held, frame->error))
		return;
	frame->columns_live = frame->held_live = true;
	doltgres_layout_values(frame->columns.ptr, frame->columns.len, frame->held.ptr, frame->held.len, &frame->set);
	frame->set_live = true;
	if (!access_slot_key_columns(row->keyed, row->keyed_len, &key, frame->error))
		return;
	frame->columns_live = false;
	access_heaptuple_RowLayout_new(&frame->columns, &key, &frame->layout);
	frame->layout_live = true;
	if (!doltgres_reform_old_values(row->tuple, row->tuple_len, row->values, row->values_len, &frame->kept,
									frame->error))
		return;
	frame->kept_live = true;
	if (!access_heaptuple_RowLayout_placing(&frame->layout, sorter, names, frame->set.ptr, frame->set.len, NULL, 0,
											false, target, &frame->placing, frame->error))
		return;
	frame->placing_live = true;
	std_vec_new(&frame->moved, DOLTGRES_REFORM_MOVED);
	frame->moved_live = true;
	while (access_heaptuple_Placing_blob(&frame->placing, &blob))
	{
		doltgres_TupleUsizeU32_t moved;
		size_t		field;
		bool		has_field = access_heaptuple_Placing_field(&frame->placing, &field);
		HostIoIo	io;
		ChunksTrailerLocator unlocated = {0};
		TreesStoreTreeRef root;
		ChunksErrorError failed;
		size_t		at;
		bool		found;

		if (blob.tag != ACCESS_HEAPTUPLE_BLOB_VALUE)
		{
			*frame->error =
				doltgres_reform_internal("a re-form that moves a row's fields into overflow chunks isn't built");
			return;
		}
		txn_blobs_Cut_Default_default(&frame->cut);
		frame->cut_live = true;
		io = pghost_io(raise);
		if (!trees_blob_write_blob(&io, TXN_BLOBS_CUT_NODE_STORE(&frame->cut), blob.u.value.ptr, blob.u.value.len,
								   TREES_CHUNKER_DEFAULT_TARGET, &root, &failed))
		{
			HostError	storage = txn_base_storage(&failed);

			*frame->error = pghost_raised(&storage);
			return;
		}
		at = doltgres_reform_kept_at(&frame->kept, root.address.f0, &found);
		if (!found)
		{
			*frame->error = doltgres_reform_internal("a re-form that moves a value out of line anew isn't built");
			return;
		}
		moved.f0 = has_field ? field : SIZE_MAX;
		moved.f1 = STD_VEC_AT(&frame->kept, DoltgresReformKept, at).id;
		std_vec_push_Checked(&frame->moved, &moved, DOLTGRES_REFORM_MOVED);
		if (!access_heaptuple_Placing_wrote(&frame->placing, root.address, unlocated, frame->error))
			return;
		txn_blobs_Cut_drop(&frame->cut);
		frame->cut_live = false;
	}
	if (frame->moved.len != frame->kept.len)
	{
		*frame->error = doltgres_reform_internal("a re-form that moves a value back in line isn't built");
		return;
	}
	std_slice_sort_unstable_TupleUsizeU32(frame->moved.ptr, frame->moved.len);
	frame->placing_live = false;
	if (!access_heaptuple_Placing_formed(&frame->placing, &frame->formed, frame->error))
		return;
	frame->formed_live = true;
	if (std_bytes_Ord_cmp(frame->formed.key.ptr, frame->formed.key.len, row->key, row->key_len) != STD_ORDERING_EQUAL)
	{
		*frame->error = doltgres_reform_internal("a re-formed row's key differs from its stored one");
		return;
	}
	if (frame->formed.heap_bytes > PG_UINT32_MAX)
	{
		*frame->error = doltgres_reform_internal("a re-formed row's heap bytes pass 32 bits");
		return;
	}
	frame->result->heap_bytes = (uint32) frame->formed.heap_bytes;
	std_vec_with_capacity_Checked(&frame->result->addresses, frame->formed.addresses.len, DOLTGRES_REFORM_U32);
	for (size_t i = 0; i < frame->formed.addresses.len; i++)
		std_vec_push_Checked(&frame->result->addresses,
							 &STD_VEC_AT(&frame->formed.addresses, chunks_TupleU32Locator_t, i).f0,
							 DOLTGRES_REFORM_U32);
	std_vec_with_capacity_Checked(&frame->result->values, frame->moved.len, DOLTGRES_REFORM_U32);
	for (size_t i = 0; i < frame->moved.len; i++)
		std_vec_push_Checked(&frame->result->values, &STD_VEC_AT(&frame->moved, doltgres_TupleUsizeU32_t, i).f1,
							 DOLTGRES_REFORM_U32);
	frame->result->tuple = frame->formed.tuple;
	std_vec_new(&frame->formed.tuple, DOLTGRES_REFORM_BYTES);
	frame->ok = true;
}

/*
 * doltgres_reform_reformed_row is the server's re-form of a fold's row,
 * which pghost's reformed door calls with the catalog capability: the row
 * read through its composed transform into a slot of its table, every value
 * whole, and placed by the catalog's layout as a fresh insert places it, its
 * values out of line kept by their content. The table is opened for the
 * row, under the lock its fold holds.
 */
bool
doltgres_reform_reformed_row(const PgCatalogCaps *caps, const HostReformRow *row, HostReformedRow *result,
							 PgPgError *error)
{
	doltgres_reform_reformed_row_frame_t frame = {.caps = caps, .row = row, .result = result, .error = error};

	std_landing(&frame, doltgres_reform_reformed_row_body, doltgres_reform_reformed_row_pad);
	doltgres_reform_reformed_row_pad(&frame);
	return frame.ok;
}

#endif							/* FRONTEND */
