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
 * Scans of a btree's entries by scan keys (access' nbtsearch.rs), Postgres'
 * nbtsearch.c, over a tree whose keys are the key encodings of the index's
 * columns, each cut at the tree's bound: each search of the index starts
 * where the scan's keys and the current elements of its arrays put it, and
 * reads on while its entries meet the keys or advance the arrays to
 * elements they meet, a gap between one combination's entries and the next
 * starting another search.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "doltgres/access/nbtsearch.h"
#include "doltgres/encoding/key.h"
#include "doltgres/pg/word.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#include "held.h"

#define RUST_FILE "access" STD_SEP "src" STD_SEP "nbtsearch.rs"

/* The element layouts of nbtsearch's vectors. */
#define ACCESS_NBTSEARCH_BYTES 1, 1
#define ACCESS_NBTSEARCH_BT_KEY sizeof(AccessNbtutilsBtKey), _Alignof(AccessNbtutilsBtKey)
#define ACCESS_NBTSEARCH_START_KEY sizeof(access_TupleBtKeyOptionFunction_t), _Alignof(access_TupleBtKeyOptionFunction_t)
#define ACCESS_NBTSEARCH_STARTS sizeof(access_OptionU32_t), _Alignof(access_OptionU32_t)
#define ACCESS_NBTSEARCH_DATUM sizeof(access_OptionU64_t), _Alignof(access_OptionU64_t)

/* Postgres' polymorphic types an opclass's operators take: anyarray, anyenum, anyrange, and anymultirange. */
#define ACCESS_NBTSEARCH_ANYARRAYOID 2277
#define ACCESS_NBTSEARCH_ANYENUMOID 3500
#define ACCESS_NBTSEARCH_ANYRANGEOID 3831
#define ACCESS_NBTSEARCH_ANYMULTIRANGEOID 4537

/* access_nbtsearch_internal returns an internal error of the fork's. */
static PgPgError
access_nbtsearch_internal(const char *message)
{
	StdString	str;

	std_string_from_Checked(&str, message, strlen(message));
	return pg_PgError_fork((const uint8 *) "XX000", &str);
}

/* access_nbtsearch_fail sets an internal error of the fork's and answers false. */
static bool
access_nbtsearch_fail(const char *message, PgPgError *error)
{
	*error = access_nbtsearch_internal(message);
	return false;
}

/* access_nbtsearch_Bound_drop frees a bound's key. */
void
access_nbtsearch_Bound_drop(access_BoundVecU8_t *self)
{
	if (self->tag != STD_BOUND_UNBOUNDED)
		std_vec_free_Checked(&self->key, ACCESS_NBTSEARCH_BYTES);
	self->tag = STD_BOUND_UNBOUNDED;
}

/* access_nbtsearch_Bound_Clone_clone copies a bound and its key. */
void
access_nbtsearch_Bound_Clone_clone(const access_BoundVecU8_t *self, access_BoundVecU8_t *result)
{
	result->tag = self->tag;
	if (self->tag != STD_BOUND_UNBOUNDED)
		std_vec_clone(&result->key, &self->key, ACCESS_NBTSEARCH_BYTES);
}

/* access_nbtsearch_KeyRange_drop frees a range's bounds. */
void
access_nbtsearch_KeyRange_drop(AccessNbtsearchKeyRange *self)
{
	access_nbtsearch_Bound_drop(&self->f0);
	access_nbtsearch_Bound_drop(&self->f1);
}

/* access_nbtsearch_unbounded returns an unbounded end of a range. */
static access_BoundVecU8_t
access_nbtsearch_unbounded(void)
{
	access_BoundVecU8_t bound = {.tag = STD_BOUND_UNBOUNDED};

	return bound;
}

/* access_nbtsearch_bound returns an end of a range of a tag and a key it takes. */
static access_BoundVecU8_t
access_nbtsearch_bound(StdBoundTag tag, StdVec key)
{
	access_BoundVecU8_t bound = {tag, key};

	return bound;
}

/*
 * access_nbtsearch_tighter returns the tighter of two bounds on one side of a
 * range, which it takes: the greater of two lower bounds, or the lesser of two
 * upper bounds.
 */
access_BoundVecU8_t
access_nbtsearch_tighter(access_BoundVecU8_t *a, access_BoundVecU8_t *b, StdOrdering side)
{
	StdOrdering order;
	bool		take_a;

	if (a->tag == STD_BOUND_UNBOUNDED)
		return *b;
	if (b->tag == STD_BOUND_UNBOUNDED)
		return *a;
	order = std_bytes_Ord_cmp(a->key.ptr, a->key.len, b->key.ptr, b->key.len);
	if (order == STD_ORDERING_EQUAL)
		take_a = a->tag == STD_BOUND_EXCLUDED;
	else
		take_a = order == side;
	access_nbtsearch_Bound_drop(take_a ? b : a);
	return take_a ? *a : *b;
}

/* access_nbtsearch_is_empty reports whether no key is within two bounds. */
bool
access_nbtsearch_is_empty(const access_BoundVecU8_t *lower, const access_BoundVecU8_t *upper)
{
	StdOrdering order;

	if (lower->tag == STD_BOUND_UNBOUNDED || upper->tag == STD_BOUND_UNBOUNDED)
		return false;
	order = std_bytes_Ord_cmp(lower->key.ptr, lower->key.len, upper->key.ptr, upper->key.len);
	if (lower->tag == STD_BOUND_INCLUDED && upper->tag == STD_BOUND_INCLUDED)
		return order == STD_ORDERING_GREATER;
	return order != STD_ORDERING_LESS;
}

/* access_nbtsearch_Position_Clone_clone copies a position and its key. */
void
access_nbtsearch_Position_Clone_clone(const AccessNbtsearchPosition *self, AccessNbtsearchPosition *result)
{
	std_vec_clone(&result->key, &self->key, ACCESS_NBTSEARCH_BYTES);
	result->copy = self->copy;
}

/* access_nbtsearch_Position_drop frees a position's key. */
void
access_nbtsearch_Position_drop(AccessNbtsearchPosition *self)
{
	std_vec_free_Checked(&self->key, ACCESS_NBTSEARCH_BYTES);
}

/* AccessNbtsearchOwnedTag is Owned's variants. */
typedef enum AccessNbtsearchOwnedTag
{
	ACCESS_NBTSEARCH_OWNED_WORD,
	ACCESS_NBTSEARCH_OWNED_BYTES,
} AccessNbtsearchOwnedTag;

/*
 * AccessNbtsearchOwned is Owned: an argument as a key's encoder takes it,
 * owning a value passed by reference's bytes.
 */
typedef struct AccessNbtsearchOwned
{
	AccessNbtsearchOwnedTag tag;
	uint64		word;
	StdVec		bytes;
} AccessNbtsearchOwned;

/* access_nbtsearch_calls returns search calls as the Calls they extend. */
static AccessNbtutilsCalls
access_nbtsearch_calls(AccessNbtsearchSearchCalls calls)
{
	AccessNbtutilsCalls as_calls = {&calls.ops->calls, calls.self};

	return as_calls;
}

/* access_nbtsearch_names returns search calls as the Names they extend. */
static AccessKeyNames
access_nbtsearch_names(AccessNbtsearchSearchCalls calls)
{
	AccessKeyNames names = {&calls.ops->names, calls.self};

	return names;
}

/* access_nbtsearch_sorter returns catalog capabilities as the SortKeys they implement. */
static AccessKeySortKeys
access_nbtsearch_sorter(const PgCatalogCaps *caps)
{
	AccessKeySortKeys sorter = {&ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS, caps};

	return sorter;
}

/*
 * access_nbtsearch_key_value returns an argument as a value of a key
 * column's type when it converts exactly, so that it encodes as a bound of
 * the column's keys: the column's own type or a polymorphic type its opclass
 * takes, another varlena text type, or another integer or float type whose
 * value the column's type holds exactly.
 */
static bool
access_nbtsearch_key_value(const PgCatalogCaps *caps, AccessNbtsearchSearchCalls calls,
						   const AccessNbtsearchKeyColumn *column, uint32 subtype, uint64 argument, bool *has_result,
						   AccessNbtsearchOwned *result, PgPgError *error)
{
	uint32		typid = column->key_type->typid;
	bool		polymorphic = false;
	bool		text_types = (typid == TEXTOID || typid == VARCHAROID) && (subtype == TEXTOID || subtype == VARCHAROID);
	bool		integers = (typid == INT2OID || typid == INT4OID || typid == INT8OID) &&
		(subtype == INT2OID || subtype == INT4OID || subtype == INT8OID);

	switch (column->key_type->inner.tag)
	{
		case ACCESS_KEY_INNER_ARRAY:
			polymorphic = subtype == ACCESS_NBTSEARCH_ANYARRAYOID;
			break;
		case ACCESS_KEY_INNER_ENUM:
			polymorphic = subtype == ACCESS_NBTSEARCH_ANYENUMOID;
			break;
		case ACCESS_KEY_INNER_RANGE:
			polymorphic = subtype == ACCESS_NBTSEARCH_ANYRANGEOID;
			break;
		case ACCESS_KEY_INNER_MULTIRANGE:
			polymorphic = subtype == ACCESS_NBTSEARCH_ANYMULTIRANGEOID;
			break;
		default:
			break;
	}
	*has_result = false;
	if (typid == subtype || polymorphic || text_types)
	{
		*has_result = true;
		if (column->by_value)
		{
			result->tag = ACCESS_NBTSEARCH_OWNED_WORD;
			result->word = argument;
			return true;
		}
		result->tag = ACCESS_NBTSEARCH_OWNED_BYTES;
		return calls.ops->bytes(calls.self, caps, argument, column->len, &result->bytes, error);
	}
	if (integers)
	{
		int64		value = subtype == INT2OID ? pg_word_to_int16(argument) :
			subtype == INT4OID ? pg_word_to_int32(argument) : pg_word_to_int64(argument);
		bool		fits = typid == INT2OID ? (value >= PG_INT16_MIN && value <= PG_INT16_MAX) :
			typid == INT4OID ? (value >= PG_INT32_MIN && value <= PG_INT32_MAX) : true;

		*has_result = fits;
		result->tag = ACCESS_NBTSEARCH_OWNED_WORD;
		result->word = (uint64) value;
		return true;
	}
	if (typid == FLOAT8OID && subtype == FLOAT4OID)
	{
		float8		value = (float8) pg_word_to_float4(argument);

		*has_result = true;
		result->tag = ACCESS_NBTSEARCH_OWNED_WORD;
		memcpy(&result->word, &value, sizeof(value));
	}
	else if (typid == FLOAT4OID && subtype == FLOAT8OID)
	{
		float8		value = pg_word_to_float8(argument);
		float4		narrowed = (float4) value;

		if (isnan(value) || (float8) narrowed == value)
		{
			uint32		bits;

			memcpy(&bits, &narrowed, sizeof(bits));
			*has_result = true;
			result->tag = ACCESS_NBTSEARCH_OWNED_WORD;
			result->word = (uint64) bits;
		}
	}
	return true;
}

/* access_nbtsearch_encode_frame_t is encode's state across its pad: the owned argument and the key it writes. */
typedef struct access_nbtsearch_encode_frame_t
{
	AccessNbtsearchOwned owned;
	bool		owned_live;
	StdVec		out;
	bool		out_live;
	const PgCatalogCaps *caps;
	AccessNbtsearchSearchCalls calls;
	const AccessNbtsearchKeyColumn *column;
	uint32		subtype;
	uint64		argument;
	size_t		bound;
	bool		compared;
	bool	   *has_result;
	bool	   *cut;
	PgPgError  *error;
	bool		ok;
} access_nbtsearch_encode_frame_t;

/* access_nbtsearch_encode_pad frees encode's owned argument and key. */
static void
access_nbtsearch_encode_pad(void *frame_)
{
	access_nbtsearch_encode_frame_t *frame = frame_;

	if (frame->owned_live && frame->owned.tag == ACCESS_NBTSEARCH_OWNED_BYTES)
		std_vec_free_Checked(&frame->owned.bytes, ACCESS_NBTSEARCH_BYTES);
	if (frame->out_live)
		std_vec_free_Checked(&frame->out, ACCESS_NBTSEARCH_BYTES);
	frame->owned_live = frame->out_live = false;
}

/*
 * access_nbtsearch_encode_body converts the argument, encodes it cut at the
 * bound, and keeps a comparator column's long argument.
 */
static void
access_nbtsearch_encode_body(void *frame_)
{
	access_nbtsearch_encode_frame_t *frame = frame_;
	AccessKeyValue value;
	bool		has;
	bool		cut;

	if (!access_nbtsearch_key_value(frame->caps, frame->calls, frame->column, frame->subtype, frame->argument, &has,
									&frame->owned, frame->error))
		return;
	*frame->has_result = has;
	if (!has)
	{
		frame->ok = true;
		return;
	}
	frame->owned_live = true;
	if (frame->owned.tag == ACCESS_NBTSEARCH_OWNED_WORD)
	{
		value.tag = ACCESS_KEY_VALUE_WORD;
		value.u.word = frame->owned.word;
	}
	else
	{
		value.tag = ACCESS_KEY_VALUE_BYTES;
		value.u.bytes.ptr = frame->owned.bytes.ptr;
		value.u.bytes.len = frame->owned.bytes.len;
	}
	std_vec_new(&frame->out, ACCESS_NBTSEARCH_BYTES);
	frame->out_live = true;
	if (!access_key_encode_within(&frame->out, access_nbtsearch_sorter(frame->caps), access_nbtsearch_names(frame->calls),
								  frame->column->key_type, true, value, frame->column->order, frame->bound,
								  frame->compared, &cut, frame->error))
		return;
	if (frame->compared)
	{
		if (cut && frame->owned.tag == ACCESS_NBTSEARCH_OWNED_BYTES && frame->out.len == 33)
		{
			StdVec		stored = frame->owned.bytes;

			frame->owned_live = false;
			if (!frame->calls.ops->probe(frame->calls.self, frame->caps, (const uint8 *) frame->out.ptr + 1, &stored,
										 frame->error))
				return;
		}
		*frame->cut = false;
		frame->ok = true;
		return;
	}
	if (cut)
		std_vec_truncate(&frame->out, frame->bound, 1, NULL);
	*frame->cut = cut;
	frame->ok = true;
}

/*
 * access_nbtsearch_encode returns the key encoding of an argument of a
 * column cut at the tree's bound, with whether it was cut, whose encoding is
 * then its first bytes up to the bound alone; or none when the argument does
 * not convert exactly. A comparator column's argument past the bound is its
 * long form, which is exact.
 */
static bool
access_nbtsearch_encode(const PgCatalogCaps *caps, AccessNbtsearchSearchCalls calls,
						const AccessNbtsearchKeyColumn *column, uint32 subtype, uint64 argument, size_t bound,
						bool compared, bool *has_result, StdVec *result, bool *cut, PgPgError *error)
{
	access_nbtsearch_encode_frame_t frame = {.caps = caps, .calls = calls, .column = column, .subtype = subtype,
		.argument = argument, .bound = bound, .compared = compared, .has_result = has_result, .cut = cut,
	.error = error};

	std_landing(&frame, access_nbtsearch_encode_body, access_nbtsearch_encode_pad);
	if (frame.ok && *has_result)
	{
		*result = frame.out;
		frame.out_live = false;
	}
	access_nbtsearch_encode_pad(&frame);
	return frame.ok;
}

/* access_nbtsearch_with_marker returns a prefix and then a marker byte, as a new vector. */
static StdVec
access_nbtsearch_with_marker(const uint8 *prefix, size_t prefix_len, uint8 marker)
{
	StdVec		out;

	std_vec_new(&out, ACCESS_NBTSEARCH_BYTES);
	std_vec_extend_from_slice_Checked(&out, prefix, prefix_len, ACCESS_NBTSEARCH_BYTES);
	std_vec_push_Checked(&out, &marker, ACCESS_NBTSEARCH_BYTES);
	return out;
}

/* access_nbtsearch_after_frame_t is after's state across its pad: the key it writes, in its held slot 0. */
typedef struct access_nbtsearch_after_frame_t
{
	access_key_held_t held;
	StdBytes	prefix;
	size_t		end;
} access_nbtsearch_after_frame_t;

/* access_nbtsearch_after_body copies the prefix through its last byte that is not 0xff and raises that byte. */
static void
access_nbtsearch_after_body(void *frame_)
{
	access_nbtsearch_after_frame_t *frame = frame_;
	StdVec	   *out = &frame->held.vec[0];

	std_range_inclusive_check(0, frame->end, frame->prefix.len, RUST_FILE, 235, 19);
	std_vec_extend_from_slice_Checked(out, frame->prefix.ptr, frame->end + 1, ACCESS_NBTSEARCH_BYTES);
	std_index_check(frame->end, out->len, RUST_FILE, 236, 5);
	((uint8 *) out->ptr)[frame->end] = STD_ADD(u8, ((uint8 *) out->ptr)[frame->end], 1, 236, 5);
}

/* access_nbtsearch_after returns the first key past every key that starts with a prefix, or false when no key is. */
static bool
access_nbtsearch_after(const uint8 *prefix, size_t prefix_len, StdVec *result)
{
	access_nbtsearch_after_frame_t frame = {.prefix = {prefix, prefix_len}};
	size_t		end = prefix_len;

	while (end > 0 && prefix[end - 1] == 0xff)
		end--;
	if (end == 0)
		return false;
	frame.end = end - 1;
	access_key_held_new(&frame.held, 0, ACCESS_NBTSEARCH_BYTES);
	std_landing(&frame, access_nbtsearch_after_body, access_key_held_pad);
	*result = access_key_held_take(&frame.held, 0);
	return true;
}

/*
 * access_nbtsearch_past returns the first key past every key that starts
 * with a prefix in a tree's order: after's, or for a tree ordered by a
 * comparator, the prefix and then PAST, which its comparator places after
 * every such key.
 */
bool
access_nbtsearch_past(const uint8 *prefix, size_t prefix_len, bool compared, StdVec *result)
{
	if (compared)
	{
		*result = access_nbtsearch_with_marker(prefix, prefix_len, ENCODING_KEY_PAST);
		return true;
	}
	return access_nbtsearch_after(prefix, prefix_len, result);
}

/*
 * access_nbtsearch_before returns the bound before every key whose columns
 * equal a prefix's in a tree's order, taking the prefix: the prefix, or for
 * a tree ordered by a comparator, the prefix and then BELOW, leaving the
 * prefix empty.
 */
static StdVec
access_nbtsearch_before(StdVec *prefix, bool compared)
{
	StdVec		out;

	if (!compared)
	{
		out = *prefix;
		std_vec_new(prefix, ACCESS_NBTSEARCH_BYTES);
		return out;
	}
	out = access_nbtsearch_with_marker(prefix->ptr, prefix->len, ENCODING_KEY_BELOW);
	std_vec_free_Checked(prefix, ACCESS_NBTSEARCH_BYTES);
	std_vec_new(prefix, ACCESS_NBTSEARCH_BYTES);
	return out;
}

/* access_nbtsearch_start_key_drop frees a start key, as a vector's element. */
static void
access_nbtsearch_start_key_drop(void *key)
{
	access_nbtutils_BtKey_drop(&((access_TupleBtKeyOptionFunction_t *) key)->f0);
}

/* access_nbtsearch_Start_drop frees a start's keys. */
static void
access_nbtsearch_Start_drop(AccessNbtsearchStart *self)
{
	std_vec_drop_Checked(&self->keys, ACCESS_NBTSEARCH_START_KEY, access_nbtsearch_start_key_drop);
}

/* access_nbtsearch_attno_column returns the column of a key's attribute number, its attno less 1. */
static size_t
access_nbtsearch_attno_column(int16 attno, uint32 line, uint32 col)
{
	return std_sub_usize((size_t) (ssize_t) attno, 1, RUST_FILE, line, col);
}

/*
 * access_nbtsearch_Start_compare returns how the start keys order against an
 * entry's values, Postgres' _bt_compare without its heap TID.
 */
static bool
access_nbtsearch_Start_compare(const AccessNbtsearchStart *self, const PgCatalogCaps *caps,
							   AccessNbtsearchSearchCalls calls, const access_OptionU64_t *values, size_t values_len,
							   int32 *result, PgPgError *error)
{
	for (size_t k = 0; k < self->keys.len; k++)
	{
		const access_TupleBtKeyOptionFunction_t *start = &STD_VEC_AT(&self->keys, access_TupleBtKeyOptionFunction_t, k);
		const AccessNbtutilsBtKey *key = &start->f0;
		bool		nulls_first = (key->flags & ACCESS_NBTUTILS_SK_BT_NULLS_FIRST) != 0;
		size_t		at = access_nbtsearch_attno_column(key->attno, 254, 36);
		bool		has_datum = at < values_len && values[at].has;
		int32		compared = 0;

		if ((key->flags & ACCESS_NBTUTILS_SK_ISNULL) != 0)
			compared = !has_datum ? 0 : nulls_first ? -1 : 1;
		else if (!has_datum)
			compared = nulls_first ? 1 : -1;
		else
		{
			if (!start->has_f1)
				return access_nbtsearch_fail("a start key without its ordering function", error);
			if (!key->has_argument)
				return access_nbtsearch_fail("a start key without its value", error);
			if (!access_nbtutils_call_order(caps, access_nbtsearch_calls(calls), start->f1, key->collation,
											values[at].value, key->argument, &compared, error))
				return false;
			if ((key->flags & ACCESS_NBTUTILS_SK_BT_DESC) == 0)
				compared = access_nbtutils_invert_compare_result(compared);
		}
		if (compared != 0)
		{
			*result = compared;
			return true;
		}
	}
	*result = 0;
	return true;
}

/*
 * access_nbtsearch_Start_precedes reports whether an entry comes before where
 * the search starts in its direction, as Postgres' _bt_binsrch places the
 * search.
 */
static bool
access_nbtsearch_Start_precedes(const AccessNbtsearchStart *self, const PgCatalogCaps *caps,
								AccessNbtsearchSearchCalls calls, const access_OptionU64_t *values, size_t values_len,
								bool backward, bool *result, PgPgError *error)
{
	int32		compared;

	if (!access_nbtsearch_Start_compare(self, caps, calls, values, values_len, &compared, error))
		return false;
	if (!backward)
		*result = self->nextkey ? compared >= 0 : compared > 0;
	else
		*result = self->nextkey ? compared < 0 : compared <= 0;
	return true;
}

/*
 * access_nbtsearch_BtScan_new prepares a scan of a btree's key columns'
 * entries, cut at the tree's bound, in the index's order or its reverse, by
 * keys Postgres preprocessed, which it takes.
 */
void
access_nbtsearch_BtScan_new(const AccessNbtsearchKeyColumn *columns, size_t columns_len, size_t bound,
							AccessNbtutilsPreprocessed *pre, bool backward, AccessNbtsearchBtScan *result)
{
	access_OptionU32_t none = {0};

	memset(result, 0, sizeof(*result));
	std_vec_new(&result->starts, ACCESS_NBTSEARCH_STARTS);
	std_vec_resize(&result->starts, columns_len, &none, ACCESS_NBTSEARCH_STARTS);
	result->bound = bound;
	for (size_t i = 0; i < columns_len && !result->compared; i++)
		result->compared = columns[i].key_type->inner.tag == ACCESS_KEY_INNER_COMPARED;
	result->pre = *pre;
	result->backward = backward;
}

/* access_nbtsearch_BtScan_drop frees what a scan holds. */
void
access_nbtsearch_BtScan_drop(AccessNbtsearchBtScan *self)
{
	access_nbtutils_Preprocessed_drop(&self->pre);
	if (self->has_current)
		access_nbtsearch_Position_drop(&self->current);
	if (self->has_resume_after)
		access_nbtsearch_Position_drop(&self->resume_after);
	if (self->has_start)
		access_nbtsearch_Start_drop(&self->start);
	std_vec_free_Checked(&self->starts, ACCESS_NBTSEARCH_STARTS);
	if (self->has_chunk)
		access_nbtsearch_KeyRange_drop(&self->chunk);
	self->has_current = self->has_resume_after = self->has_start = self->has_chunk = false;
}

/* access_nbtsearch_BtScan_dir returns the scan's direction. */
static AccessNbtutilsScanDirection
access_nbtsearch_BtScan_dir(const AccessNbtsearchBtScan *self)
{
	return self->backward ? ACCESS_NBTUTILS_SCAN_DIRECTION_BACKWARD : ACCESS_NBTUTILS_SCAN_DIRECTION_FORWARD;
}

/* access_nbtsearch_BtScan_checker returns the checker of the scan's keys in its direction. */
static AccessNbtutilsChecker
access_nbtsearch_BtScan_checker(AccessNbtsearchBtScan *self, AccessNbtsearchSearchCalls calls)
{
	AccessNbtutilsChecker checker = {&self->pre, access_nbtsearch_calls(calls), access_nbtsearch_BtScan_dir(self),
	&self->need_prim_scan};

	return checker;
}

/*
 * access_nbtsearch_BtScan_first_closure_7 is first's positioning: whether a
 * strategy's search starts past the entries equal to its keys, and whether it
 * reads backward.
 */
static void
access_nbtsearch_BtScan_first_closure_7(bool forward, int16 strategy, bool *nextkey, bool *backward)
{
	switch (strategy)
	{
		case ACCESS_NBTUTILS_BT_LESS_STRATEGY_NUMBER:
			*nextkey = false;
			*backward = true;
			return;
		case ACCESS_NBTUTILS_BT_LESS_EQUAL_STRATEGY_NUMBER:
			*nextkey = true;
			*backward = true;
			return;
		case ACCESS_NBTUTILS_BT_EQUAL_STRATEGY_NUMBER:
			*nextkey = !forward;
			*backward = !forward;
			return;
		case ACCESS_NBTUTILS_BT_GREATER_EQUAL_STRATEGY_NUMBER:
			*nextkey = false;
			*backward = false;
			return;
	}
	*nextkey = true;
	*backward = false;
}

/*
 * access_nbtsearch_first_frame_t is first's state across its pad: the start
 * keys (a Vec<BtKey>) from a position on, the key chosen for an attribute,
 * a row header's members not yet taken, the encoded prefix and the
 * encoding of a key, the keys of the start with their ordering functions,
 * and the start.
 */
typedef struct access_nbtsearch_first_frame_t
{
	StdVec		start_keys;
	size_t		start_keys_next;
	bool		has_chosen;
	AccessNbtutilsBtKey chosen;
	bool		members_live;
	StdVec		members;
	size_t		members_next;
	StdVec		prefix;
	bool		has_encoded;
	StdVec		encoded;
	bool		keys_live;
	StdVec		keys;
	bool		has_start;
	AccessNbtsearchStart start;
	AccessNbtsearchBtScan *self;
	const PgCatalogCaps *caps;
	const AccessNbtsearchKeyColumn *columns;
	size_t		columns_len;
	AccessNbtsearchSearchCalls calls;
	bool		backward;
	bool	   *has_result;
	AccessNbtsearchKeyRange *bounds;
	bool	   *has_start_result;
	AccessNbtsearchStart *start_result;
	PgPgError  *error;
	bool		ok;
} access_nbtsearch_first_frame_t;

/* access_nbtsearch_drop_from frees a vector's keys from a position on, and the vector. */
static void
access_nbtsearch_drop_from(StdVec *keys, size_t next)
{
	for (size_t i = next; i < keys->len; i++)
		access_nbtutils_BtKey_drop(&STD_VEC_AT(keys, AccessNbtutilsBtKey, i));
	std_vec_free_Checked(keys, ACCESS_NBTSEARCH_BT_KEY);
}

/* access_nbtsearch_BtScan_first_pad frees what first holds. */
static void
access_nbtsearch_BtScan_first_pad(void *frame_)
{
	access_nbtsearch_first_frame_t *frame = frame_;

	access_nbtsearch_drop_from(&frame->start_keys, frame->start_keys_next);
	frame->start_keys_next = 0;
	if (frame->has_chosen)
		access_nbtutils_BtKey_drop(&frame->chosen);
	if (frame->members_live)
		access_nbtsearch_drop_from(&frame->members, frame->members_next);
	std_vec_free_Checked(&frame->prefix, ACCESS_NBTSEARCH_BYTES);
	if (frame->has_encoded)
		std_vec_free_Checked(&frame->encoded, ACCESS_NBTSEARCH_BYTES);
	if (frame->keys_live)
		std_vec_drop_Checked(&frame->keys, ACCESS_NBTSEARCH_START_KEY, access_nbtsearch_start_key_drop);
	if (frame->has_start)
		access_nbtsearch_Start_drop(&frame->start);
	frame->has_chosen = frame->members_live = frame->has_encoded = frame->keys_live = frame->has_start = false;
}

/*
 * access_nbtsearch_set_chosen makes a key, which it takes, the key chosen for
 * the attribute, freeing the one chosen before.
 */
static void
access_nbtsearch_set_chosen(access_nbtsearch_first_frame_t *frame, const AccessNbtutilsBtKey *key)
{
	if (frame->has_chosen)
		access_nbtutils_BtKey_drop(&frame->chosen);
	frame->chosen = *key;
	frame->has_chosen = true;
}

/*
 * access_nbtsearch_BtScan_first_choose chooses the start keys, Postgres'
 * _bt_first choosing the keys of its insertion scan key, and answers its total
 * strategy, or false on an error.
 */
static bool
access_nbtsearch_BtScan_first_choose(access_nbtsearch_first_frame_t *frame, int16 *strat_total)
{
	AccessNbtsearchBtScan *self = frame->self;
	const StdVec *keys = &self->pre.keys;
	bool		forward = !frame->backward;
	int16		curattr = 1;
	bool		has_bkey = false;
	size_t		bkey = 0;
	bool		has_implies_nn = false;
	size_t		implies_nn = 0;
	size_t		i = 0;

	if (keys->len == 0)
		return true;
	for (;;)
	{
		if (i < keys->len)
			std_index_check(i, keys->len, RUST_FILE, 506, 39);
		if (i >= keys->len || STD_VEC_AT(keys, AccessNbtutilsBtKey, i).attno != curattr)
		{
			AccessNbtutilsBtKey chosen;
			bool		next;

			if (has_bkey)
			{
				std_index_check(bkey, keys->len, RUST_FILE, 507, 51);
				access_nbtutils_BtKey_Clone_clone(&STD_VEC_AT(keys, AccessNbtutilsBtKey, bkey), &chosen);
				access_nbtsearch_set_chosen(frame, &chosen);
				std_index_check(bkey, keys->len, RUST_FILE, 508, 55);
			}
			if (has_bkey && (STD_VEC_AT(keys, AccessNbtutilsBtKey, bkey).flags &
							 (ACCESS_NBTUTILS_SK_BT_MINVAL | ACCESS_NBTUTILS_SK_BT_MAXVAL)) != 0)
			{
				const AccessNbtutilsArrayKey *array = NULL;
				const AccessNbtutilsBtKey *compare;
				bool		has_compare;

				for (size_t a = 0; a < self->pre.arrays.len && array == NULL; a++)
					if (STD_VEC_AT(&self->pre.arrays, AccessNbtutilsArrayKey, a).scan_key == bkey)
						array = &STD_VEC_AT(&self->pre.arrays, AccessNbtutilsArrayKey, a);
				if (array == NULL)
					return access_nbtsearch_fail("a skip array key without its array", frame->error);
				compare = forward ? &array->low_compare : &array->high_compare;
				has_compare = forward ? array->has_low_compare : array->has_high_compare;
				if (frame->has_chosen)
					access_nbtutils_BtKey_drop(&frame->chosen);
				frame->has_chosen = false;
				if (has_compare)
				{
					access_nbtutils_BtKey_Clone_clone(compare, &chosen);
					access_nbtsearch_set_chosen(frame, &chosen);
				}
				if (!array->null_elem)
				{
					has_implies_nn = true;
					implies_nn = bkey;
				}
			}
			if (!frame->has_chosen && has_implies_nn)
			{
				bool		nulls_first;

				std_index_check(implies_nn, keys->len, RUST_FILE, 526, 43);
				nulls_first = (STD_VEC_AT(keys, AccessNbtutilsBtKey, implies_nn).flags &
							   ACCESS_NBTUTILS_SK_BT_NULLS_FIRST) != 0;
				if (nulls_first == forward)
				{
					AccessNbtutilsBtKey made = {0};

					std_index_check(implies_nn, keys->len, RUST_FILE, 531, 40);
					made.flags = ACCESS_NBTUTILS_SK_SEARCHNOTNULL | ACCESS_NBTUTILS_SK_ISNULL |
						(STD_VEC_AT(keys, AccessNbtutilsBtKey, implies_nn).flags &
						 (ACCESS_NBTUTILS_SK_BT_DESC | ACCESS_NBTUTILS_SK_BT_NULLS_FIRST));
					made.attno = curattr;
					made.strategy = nulls_first ? ACCESS_NBTUTILS_BT_GREATER_STRATEGY_NUMBER :
						ACCESS_NBTUTILS_BT_LESS_STRATEGY_NUMBER;
					made.func.tag = ACCESS_NBTUTILS_FUNCTION_START;
					made.func.f0 = access_nbtsearch_attno_column(curattr, 541, 55);
					std_vec_new(&made.members, ACCESS_NBTSEARCH_BT_KEY);
					access_nbtsearch_set_chosen(frame, &made);
				}
			}
			if (!frame->has_chosen)
				break;
			*strat_total = frame->chosen.strategy;
			next = (frame->chosen.flags & (ACCESS_NBTUTILS_SK_BT_NEXT | ACCESS_NBTUTILS_SK_BT_PRIOR)) != 0;
			std_vec_push_Checked(&frame->start_keys, &frame->chosen, ACCESS_NBTSEARCH_BT_KEY);
			frame->has_chosen = false;
			if (*strat_total == ACCESS_NBTUTILS_BT_GREATER_STRATEGY_NUMBER ||
				*strat_total == ACCESS_NBTUTILS_BT_LESS_STRATEGY_NUMBER)
				break;
			if (next)
			{
				*strat_total = forward ? ACCESS_NBTUTILS_BT_GREATER_STRATEGY_NUMBER :
					ACCESS_NBTUTILS_BT_LESS_STRATEGY_NUMBER;
				break;
			}
			if (i < keys->len)
				std_index_check(i, keys->len, RUST_FILE, 557, 43);
			if (i >= keys->len || (STD_VEC_AT(keys, AccessNbtutilsBtKey, i).flags &
								   (ACCESS_NBTUTILS_SK_BT_REQFWD | ACCESS_NBTUTILS_SK_BT_REQBKWD)) == 0)
				break;
			std_index_check(i, keys->len, RUST_FILE, 560, 31);
			curattr = STD_VEC_AT(keys, AccessNbtutilsBtKey, i).attno;
			has_bkey = has_implies_nn = false;
		}
		if (has_bkey)
		{
			i = STD_ADD(usize, i, 1, 564, 21);
			continue;
		}
		std_index_check(i, keys->len, RUST_FILE, 567, 23);
		switch (STD_VEC_AT(keys, AccessNbtutilsBtKey, i).strategy)
		{
			case ACCESS_NBTUTILS_BT_LESS_STRATEGY_NUMBER:
			case ACCESS_NBTUTILS_BT_LESS_EQUAL_STRATEGY_NUMBER:
				if (!forward)
				{
					has_bkey = true;
					bkey = i;
				}
				else if (!has_implies_nn)
				{
					has_implies_nn = true;
					implies_nn = i;
				}
				break;
			case ACCESS_NBTUTILS_BT_EQUAL_STRATEGY_NUMBER:
				has_bkey = true;
				bkey = i;
				break;
			default:
				if (forward)
				{
					has_bkey = true;
					bkey = i;
				}
				else if (!has_implies_nn)
				{
					has_implies_nn = true;
					implies_nn = i;
				}
				break;
		}
		i = STD_ADD(usize, i, 1, 584, 17);
	}
	return true;
}

/*
 * access_nbtsearch_BtScan_first_row takes a row header ending the start keys
 * apart into its members, as _bt_first does, and answers the total strategy it
 * leaves.
 */
static void
access_nbtsearch_BtScan_first_row(access_nbtsearch_first_frame_t *frame, int16 *strat_total)
{
	AccessNbtutilsBtKey *last;
	bool		loosen = false;
	bool		tighten = false;

	if (frame->start_keys.len == 0)
		return;
	last = &STD_VEC_AT(&frame->start_keys, AccessNbtutilsBtKey, frame->start_keys.len - 1);
	if ((last->flags & ACCESS_NBTUTILS_SK_ROW_HEADER) == 0)
		return;
	frame->members = last->members;
	frame->members_next = 0;
	frame->members_live = true;
	frame->start_keys.len--;
	if (frame->members_next < frame->members.len)
	{
		std_vec_push_Checked(&frame->start_keys, &STD_VEC_AT(&frame->members, AccessNbtutilsBtKey, frame->members_next),
							 ACCESS_NBTSEARCH_BT_KEY);
		frame->members_next++;
	}
	while (frame->members_next < frame->members.len)
	{
		AccessNbtutilsBtKey *member = &STD_VEC_AT(&frame->members, AccessNbtutilsBtKey, frame->members_next);

		if ((member->flags & ACCESS_NBTUTILS_SK_ISNULL) != 0)
		{
			tighten = true;
			break;
		}
		if ((member->flags & (ACCESS_NBTUTILS_SK_BT_REQFWD | ACCESS_NBTUTILS_SK_BT_REQBKWD)) == 0)
		{
			loosen = true;
			break;
		}
		std_vec_push_Checked(&frame->start_keys, member, ACCESS_NBTSEARCH_BT_KEY);
		frame->members_next++;
	}
	access_nbtsearch_drop_from(&frame->members, frame->members_next);
	frame->members_live = false;
	if (*strat_total == ACCESS_NBTUTILS_BT_LESS_STRATEGY_NUMBER && loosen)
		*strat_total = ACCESS_NBTUTILS_BT_LESS_EQUAL_STRATEGY_NUMBER;
	else if (*strat_total == ACCESS_NBTUTILS_BT_GREATER_STRATEGY_NUMBER && loosen)
		*strat_total = ACCESS_NBTUTILS_BT_GREATER_EQUAL_STRATEGY_NUMBER;
	else if (*strat_total == ACCESS_NBTUTILS_BT_LESS_EQUAL_STRATEGY_NUMBER && tighten)
		*strat_total = ACCESS_NBTUTILS_BT_LESS_STRATEGY_NUMBER;
	else if (*strat_total == ACCESS_NBTUTILS_BT_GREATER_EQUAL_STRATEGY_NUMBER && tighten)
		*strat_total = ACCESS_NBTUTILS_BT_GREATER_STRATEGY_NUMBER;
}

/*
 * access_nbtsearch_BtScan_first_body chooses the start keys, encodes their
 * prefix, and makes the search's bounds and its start.
 */
static void
access_nbtsearch_BtScan_first_body(void *frame_)
{
	access_nbtsearch_first_frame_t *frame = frame_;
	AccessNbtsearchBtScan *self = frame->self;
	bool		forward = !frame->backward;
	int16		strat_total = ACCESS_NBTUTILS_BT_EQUAL_STRATEGY_NUMBER;
	bool		precise = true;
	bool		nextkey;
	bool		backward;
	StdVec		past;

	if (!access_nbtsearch_BtScan_first_choose(frame, &strat_total))
		return;
	access_nbtsearch_BtScan_first_row(frame, &strat_total);
	for (size_t k = 0; k < frame->start_keys.len; k++)
	{
		const AccessNbtutilsBtKey *key = &STD_VEC_AT(&frame->start_keys, AccessNbtutilsBtKey, k);
		size_t		at = access_nbtsearch_attno_column(key->attno, 614, 29);
		const AccessNbtsearchKeyColumn *column;
		AccessKeyValue none = {0};
		bool		has_encoded = false;
		bool		cut = false;

		if (at >= frame->columns_len)
		{
			access_nbtsearch_fail("a key past the index's columns", frame->error);
			return;
		}
		column = &frame->columns[at];
		if ((key->flags & ACCESS_NBTUTILS_SK_ISNULL) != 0)
		{
			std_vec_new(&frame->encoded, ACCESS_NBTSEARCH_BYTES);
			frame->has_encoded = true;
			if (!access_key_encode(&frame->encoded, access_nbtsearch_sorter(frame->caps), column->key_type, false, none,
								   column->order, frame->error))
				return;
			has_encoded = true;
		}
		else
		{
			uint32		subtype;

			if (!key->has_argument)
			{
				access_nbtsearch_fail("a start key without its value", frame->error);
				return;
			}
			subtype = key->subtype == 0 ? column->key_type->typid : key->subtype;
			if ((column->collation == 0 || key->collation == column->collation) &&
				!access_nbtsearch_encode(frame->caps, frame->calls, column, subtype, key->argument, self->bound,
										 self->compared, &has_encoded, &frame->encoded, &cut, frame->error))
				return;
			frame->has_encoded = has_encoded;
		}
		if (!has_encoded)
		{
			precise = false;
			break;
		}
		std_vec_extend_from_slice_Checked(&frame->prefix, frame->encoded.ptr, frame->encoded.len,
										  ACCESS_NBTSEARCH_BYTES);
		std_vec_free_Checked(&frame->encoded, ACCESS_NBTSEARCH_BYTES);
		frame->has_encoded = false;
		if (cut)
		{
			precise = false;
			break;
		}
	}
	if (!precise)
	{
		std_vec_with_capacity_Checked(&frame->keys, frame->start_keys.len, ACCESS_NBTSEARCH_START_KEY);
		frame->keys_live = true;
		while (frame->start_keys_next < frame->start_keys.len)
		{
			access_TupleBtKeyOptionFunction_t start = {STD_VEC_AT(&frame->start_keys, AccessNbtutilsBtKey,
																  frame->start_keys_next)};
			size_t		column = access_nbtsearch_attno_column(start.f0.attno, 653, 34);

			if ((start.f0.flags & ACCESS_NBTUTILS_SK_ISNULL) == 0)
			{
				access_OptionU32_t *ready;

				std_index_check(column, self->starts.len, RUST_FILE, 657, 32);
				ready = &STD_VEC_AT(&self->starts, access_OptionU32_t, column);
				if (!(ready->has && ready->value == start.f0.subtype))
				{
					if (!frame->calls.ops->start_order(frame->calls.self, frame->caps, column, start.f0.subtype,
													   frame->error))
						return;
					std_index_check(column, self->starts.len, RUST_FILE, 659, 33);
					ready->has = true;
					ready->value = start.f0.subtype;
				}
				start.has_f1 = true;
				start.f1.tag = ACCESS_NBTUTILS_FUNCTION_START;
				start.f1.f0 = column;
			}
			std_vec_push_Checked(&frame->keys, &start, ACCESS_NBTSEARCH_START_KEY);
			frame->start_keys_next++;
		}
		access_nbtsearch_BtScan_first_closure_7(forward, strat_total, &nextkey, &backward);
		strat_total = ACCESS_NBTUTILS_BT_EQUAL_STRATEGY_NUMBER;
		frame->start.keys = frame->keys;
		frame->start.nextkey = nextkey;
		frame->keys_live = false;
		frame->has_start = true;
	}
	*frame->has_start_result = frame->has_start;
	if (frame->prefix.len == 0)
	{
		frame->bounds->f0 = access_nbtsearch_unbounded();
		frame->bounds->f1 = access_nbtsearch_unbounded();
		*frame->has_result = true;
		frame->ok = true;
		return;
	}
	access_nbtsearch_BtScan_first_closure_7(forward, strat_total, &nextkey, &backward);
	if (!backward && !nextkey)
	{
		frame->bounds->f0 = access_nbtsearch_bound(STD_BOUND_INCLUDED, access_nbtsearch_before(&frame->prefix,
																							   self->compared));
		frame->bounds->f1 = access_nbtsearch_unbounded();
	}
	else if (!backward)
	{
		if (!access_nbtsearch_past(frame->prefix.ptr, frame->prefix.len, self->compared, &past))
		{
			*frame->has_result = false;
			frame->ok = true;
			return;
		}
		frame->bounds->f0 = access_nbtsearch_bound(STD_BOUND_INCLUDED, past);
		frame->bounds->f1 = access_nbtsearch_unbounded();
	}
	else if (!nextkey)
	{
		frame->bounds->f0 = access_nbtsearch_unbounded();
		frame->bounds->f1 = access_nbtsearch_bound(STD_BOUND_EXCLUDED, access_nbtsearch_before(&frame->prefix,
																							   self->compared));
	}
	else
	{
		frame->bounds->f0 = access_nbtsearch_unbounded();
		frame->bounds->f1 = access_nbtsearch_past(frame->prefix.ptr, frame->prefix.len, self->compared, &past) ?
			access_nbtsearch_bound(STD_BOUND_EXCLUDED, past) : access_nbtsearch_unbounded();
	}
	*frame->has_result = true;
	frame->ok = true;
}

/*
 * access_nbtsearch_BtScan_first returns the keys a search of the index
 * reads, from where the scan's keys put its first entry in a direction to
 * the end of the index that way, with where it starts when the keys read
 * begin before that, or none when no key can be there, Postgres' _bt_first
 * choosing the keys of its insertion scan key and positioning the scan with
 * _bt_search.
 */
static bool
access_nbtsearch_BtScan_first(AccessNbtsearchBtScan *self, const PgCatalogCaps *caps,
							  const AccessNbtsearchKeyColumn *columns, size_t columns_len,
							  AccessNbtsearchSearchCalls calls, bool backward, bool *has_result,
							  AccessNbtsearchKeyRange *bounds, bool *has_start, AccessNbtsearchStart *start,
							  PgPgError *error)
{
	access_nbtsearch_first_frame_t frame = {.self = self, .caps = caps, .columns = columns, .columns_len = columns_len,
		.calls = calls, .backward = backward, .has_result = has_result, .bounds = bounds,
	.has_start_result = has_start, .start_result = start, .error = error};

	std_vec_new(&frame.start_keys, ACCESS_NBTSEARCH_BT_KEY);
	std_vec_new(&frame.prefix, ACCESS_NBTSEARCH_BYTES);
	std_landing(&frame, access_nbtsearch_BtScan_first_body, access_nbtsearch_BtScan_first_pad);
	if (frame.ok && *has_result && frame.has_start)
	{
		*start = frame.start;
		frame.has_start = false;
	}
	else
		*has_start = false;
	access_nbtsearch_BtScan_first_pad(&frame);
	return frame.ok;
}

/*
 * access_nbtsearch_reach_frame_t is reach's state across its pad: the bounds
 * it copies, the far end's bounds first gives, and the start it gives with
 * them.
 */
typedef struct access_nbtsearch_reach_frame_t
{
	AccessNbtsearchKeyRange bounds;
	AccessNbtsearchKeyRange ends;
	bool		has_start;
	AccessNbtsearchStart start;
	AccessNbtsearchBtScan *self;
	const PgCatalogCaps *caps;
	const AccessNbtsearchKeyColumn *columns;
	size_t		columns_len;
	AccessNbtsearchSearchCalls calls;
	const access_BoundVecU8_t *lower;
	const access_BoundVecU8_t *upper;
	AccessNbtsearchKeyRange *result;
	PgPgError  *error;
	bool		ok;
} access_nbtsearch_reach_frame_t;

/* access_nbtsearch_BtScan_reach_pad frees what reach holds. */
static void
access_nbtsearch_BtScan_reach_pad(void *frame_)
{
	access_nbtsearch_reach_frame_t *frame = frame_;

	access_nbtsearch_KeyRange_drop(&frame->bounds);
	access_nbtsearch_KeyRange_drop(&frame->ends);
	if (frame->has_start)
		access_nbtsearch_Start_drop(&frame->start);
	frame->has_start = false;
}

/* access_nbtsearch_BtScan_reach_body bounds a search without arrays by where the scan's keys end it. */
static void
access_nbtsearch_BtScan_reach_body(void *frame_)
{
	access_nbtsearch_reach_frame_t *frame = frame_;
	AccessNbtsearchBtScan *self = frame->self;
	bool		has;

	access_nbtsearch_Bound_Clone_clone(frame->lower, &frame->bounds.f0);
	access_nbtsearch_Bound_Clone_clone(frame->upper, &frame->bounds.f1);
	if (self->pre.arrays.len == 0)
	{
		if (!access_nbtsearch_BtScan_first(self, frame->caps, frame->columns, frame->columns_len, frame->calls,
										   !self->backward, &has, &frame->ends, &frame->has_start, &frame->start,
										   frame->error))
			return;
		if (has && frame->bounds.f1.tag == STD_BOUND_UNBOUNDED && !self->backward)
		{
			frame->bounds.f1 = frame->ends.f1;
			frame->ends.f1 = access_nbtsearch_unbounded();
		}
		else if (has && frame->bounds.f0.tag == STD_BOUND_UNBOUNDED && self->backward)
		{
			frame->bounds.f0 = frame->ends.f0;
			frame->ends.f0 = access_nbtsearch_unbounded();
		}
	}
	*frame->result = frame->bounds;
	frame->bounds.f0 = access_nbtsearch_unbounded();
	frame->bounds.f1 = access_nbtsearch_unbounded();
	frame->ok = true;
}

/*
 * access_nbtsearch_BtScan_reach returns the keys a search between bounds can
 * read: up to where the scan's keys end it when it has no arrays, whose
 * elements can carry one search past that, so that it reads no version of a
 * key it can't return.
 */
static bool
access_nbtsearch_BtScan_reach(AccessNbtsearchBtScan *self, const PgCatalogCaps *caps,
							  const AccessNbtsearchKeyColumn *columns, size_t columns_len,
							  AccessNbtsearchSearchCalls calls, const access_BoundVecU8_t *lower,
							  const access_BoundVecU8_t *upper, AccessNbtsearchKeyRange *result, PgPgError *error)
{
	access_nbtsearch_reach_frame_t frame = {.self = self, .caps = caps, .columns = columns, .columns_len = columns_len,
	.calls = calls, .lower = lower, .upper = upper, .result = result, .error = error};

	frame.bounds.f0 = frame.bounds.f1 = frame.ends.f0 = frame.ends.f1 = access_nbtsearch_unbounded();
	std_landing(&frame, access_nbtsearch_BtScan_reach_body, access_nbtsearch_BtScan_reach_pad);
	access_nbtsearch_BtScan_reach_pad(&frame);
	return frame.ok;
}

/*
 * access_nbtsearch_next_frame_t is next_entry's state across its pad: a
 * search's bounds and the bounds it reaches, the position it resumes after,
 * and the position and values of the entry it read last.
 */
typedef struct access_nbtsearch_next_frame_t
{
	AccessNbtsearchKeyRange bounds;
	AccessNbtsearchKeyRange reached;
	bool		has_after;
	AccessNbtsearchPosition after;
	bool		has_position;
	AccessNbtsearchPosition position;
	bool		values_live;
	StdVec		values;
	AccessNbtsearchBtScan *self;
	const PgCatalogCaps *caps;
	const AccessNbtsearchKeyColumn *columns;
	size_t		columns_len;
	AccessNbtsearchReader reader;
	AccessNbtsearchSearchCalls calls;
	bool	   *result;
	PgPgError  *error;
	bool		ok;
} access_nbtsearch_next_frame_t;

/* access_nbtsearch_next_search_drop frees a search's bounds and the position it resumed after. */
static void
access_nbtsearch_next_search_drop(access_nbtsearch_next_frame_t *frame)
{
	access_nbtsearch_KeyRange_drop(&frame->reached);
	access_nbtsearch_KeyRange_drop(&frame->bounds);
	if (frame->has_after)
		access_nbtsearch_Position_drop(&frame->after);
	frame->has_after = false;
}

/* access_nbtsearch_next_entry_drop frees the entry read last's values and position. */
static void
access_nbtsearch_next_entry_drop(access_nbtsearch_next_frame_t *frame)
{
	if (frame->values_live)
		std_vec_free_Checked(&frame->values, ACCESS_NBTSEARCH_DATUM);
	if (frame->has_position)
		access_nbtsearch_Position_drop(&frame->position);
	frame->values_live = frame->has_position = false;
}

/* access_nbtsearch_BtScan_next_entry_pad frees what next_entry holds. */
static void
access_nbtsearch_BtScan_next_entry_pad(void *frame_)
{
	access_nbtsearch_next_frame_t *frame = frame_;

	access_nbtsearch_next_search_drop(frame);
	access_nbtsearch_next_entry_drop(frame);
}

/* access_nbtsearch_next_end ends next_entry's scan with no entry. */
static void
access_nbtsearch_next_end(access_nbtsearch_next_frame_t *frame)
{
	frame->self->ended = true;
	*frame->result = false;
	frame->ok = true;
}

/*
 * access_nbtsearch_BtScan_next_entry_search starts a search where the scan's
 * keys or its resumed position put it, or ends the scan.
 */
static bool
access_nbtsearch_BtScan_next_entry_search(access_nbtsearch_next_frame_t *frame, bool *ended)
{
	AccessNbtsearchBtScan *self = frame->self;
	access_BoundVecU8_t chunk;

	*ended = false;
	if (self->has_start)
		access_nbtsearch_Start_drop(&self->start);
	self->has_start = false;
	frame->has_after = self->has_resume_after;
	frame->after = self->resume_after;
	self->has_resume_after = false;
	if (frame->has_after)
	{
		StdVec		key;

		std_vec_clone(&key, &frame->after.key, ACCESS_NBTSEARCH_BYTES);
		frame->bounds.f0 = self->backward ? access_nbtsearch_unbounded() :
			access_nbtsearch_bound(STD_BOUND_EXCLUDED, key);
		frame->bounds.f1 = self->backward ? access_nbtsearch_bound(STD_BOUND_EXCLUDED, key) :
			access_nbtsearch_unbounded();
	}
	else
	{
		bool		has;

		if ((self->started && !self->need_prim_scan) || !self->pre.qual_ok)
		{
			*ended = true;
			return true;
		}
		if (!self->started && self->pre.arrays.len != 0)
		{
			AccessNbtutilsChecker checker = access_nbtsearch_BtScan_checker(self, frame->calls);

			access_nbtutils_Checker_start_array_keys(&checker, access_nbtsearch_BtScan_dir(self));
		}
		self->started = true;
		self->need_prim_scan = false;
		self->nsearches = STD_ADD(u64, self->nsearches, 1, 402, 25);
		if (!access_nbtsearch_BtScan_first(self, frame->caps, frame->columns, frame->columns_len, frame->calls,
										   self->backward, &has, &frame->bounds, &self->has_start, &self->start,
										   frame->error))
			return false;
		if (!has)
		{
			*ended = true;
			return true;
		}
	}
	self->started = true;
	if (self->has_chunk)
	{
		access_nbtsearch_Bound_Clone_clone(&self->chunk.f0, &chunk);
		frame->bounds.f0 = access_nbtsearch_tighter(&frame->bounds.f0, &chunk, STD_ORDERING_GREATER);
		access_nbtsearch_Bound_Clone_clone(&self->chunk.f1, &chunk);
		frame->bounds.f1 = access_nbtsearch_tighter(&frame->bounds.f1, &chunk, STD_ORDERING_LESS);
	}
	if (access_nbtsearch_is_empty(&frame->bounds.f0, &frame->bounds.f1))
	{
		*ended = true;
		return true;
	}
	if (!access_nbtsearch_BtScan_reach(self, frame->caps, frame->columns, frame->columns_len, frame->calls,
									   &frame->bounds.f0, &frame->bounds.f1, &frame->reached, frame->error))
		return false;
	if (!frame->reader.ops->search(frame->reader.self, frame->caps, &frame->reached.f0, &frame->reached.f1,
								   self->backward, frame->has_after ? &frame->after : NULL, frame->error))
		return false;
	self->searching = true;
	access_nbtsearch_next_search_drop(frame);
	return true;
}

/* access_nbtsearch_BtScan_next_entry_body reads on to the next entry that meets every key. */
static void
access_nbtsearch_BtScan_next_entry_body(void *frame_)
{
	access_nbtsearch_next_frame_t *frame = frame_;
	AccessNbtsearchBtScan *self = frame->self;

	for (;;)
	{
		bool		has;
		bool		continuescan = true;
		bool		passes;
		AccessNbtutilsChecker checker;

		if (!self->searching)
		{
			bool		ended;

			if (self->ended)
			{
				*frame->result = false;
				frame->ok = true;
				return;
			}
			if (!access_nbtsearch_BtScan_next_entry_search(frame, &ended))
				return;
			if (ended)
			{
				access_nbtsearch_next_end(frame);
				return;
			}
		}
		if (!frame->reader.ops->next(frame->reader.self, frame->caps, &has, &frame->position, frame->error))
			return;
		if (!has)
		{
			self->searching = false;
			if (!self->need_prim_scan)
				self->ended = true;
			continue;
		}
		frame->has_position = true;
		if (!frame->reader.ops->values(frame->reader.self, frame->caps, &frame->values, frame->error))
			return;
		frame->values_live = true;
		if (self->has_start)
		{
			bool		precedes;

			if (!access_nbtsearch_Start_precedes(&self->start, frame->caps, frame->calls, frame->values.ptr,
												 frame->values.len, self->backward, &precedes, frame->error))
				return;
			if (precedes)
			{
				access_nbtsearch_next_entry_drop(frame);
				continue;
			}
			access_nbtsearch_Start_drop(&self->start);
			self->has_start = false;
		}
		checker = access_nbtsearch_BtScan_checker(self, frame->calls);
		if (!access_nbtutils_Checker_checkkeys(&checker, frame->caps, frame->values.ptr, frame->values.len,
											   self->pre.arrays.len != 0, &continuescan, &passes, frame->error))
			return;
		if (passes)
		{
			if (self->has_current)
				access_nbtsearch_Position_drop(&self->current);
			self->current = frame->position;
			self->has_current = true;
			frame->has_position = false;
			access_nbtsearch_next_entry_drop(frame);
			*frame->result = true;
			frame->ok = true;
			return;
		}
		if (!continuescan)
			self->searching = false;
		access_nbtsearch_next_entry_drop(frame);
	}
}

/*
 * access_nbtsearch_BtScan_next_entry reads on to the next entry that meets
 * every key, which the reader keeps, and reports whether there was one,
 * Postgres' btgettuple with _bt_first, _bt_readpage, and _bt_checkkeys:
 * each search of the index starts where the arrays' current elements put it
 * and reads on while the entries it reads meet the keys or advance the
 * arrays to elements they meet.
 */
bool
access_nbtsearch_BtScan_next_entry(AccessNbtsearchBtScan *self, const PgCatalogCaps *caps,
								   const AccessNbtsearchKeyColumn *columns, size_t columns_len,
								   AccessNbtsearchReader reader, AccessNbtsearchSearchCalls calls, bool *result,
								   PgPgError *error)
{
	access_nbtsearch_next_frame_t frame = {.self = self, .caps = caps, .columns = columns, .columns_len = columns_len,
	.reader = reader, .calls = calls, .result = result, .error = error};

	frame.bounds.f0 = frame.bounds.f1 = frame.reached.f0 = frame.reached.f1 = access_nbtsearch_unbounded();
	std_landing(&frame, access_nbtsearch_BtScan_next_entry_body, access_nbtsearch_BtScan_next_entry_pad);
	access_nbtsearch_BtScan_next_entry_pad(&frame);
	return frame.ok;
}

/*
 * access_nbtsearch_BtScan_set_direction turns the scan to read in a
 * direction, from the entry it returned last, or from the far end once it
 * had found no more entries, and answers whether it starts from the far
 * end, or false when it already reads that way.
 */
bool
access_nbtsearch_BtScan_set_direction(AccessNbtsearchBtScan *self, bool backward, bool *result)
{
	bool		restart;

	if (backward == self->backward)
		return false;
	restart = self->ended || !self->has_current;
	self->backward = backward;
	self->searching = false;
	self->need_prim_scan = false;
	self->ended = false;
	if (self->has_resume_after)
		access_nbtsearch_Position_drop(&self->resume_after);
	self->has_resume_after = false;
	if (restart)
	{
		self->started = false;
		if (self->has_current)
			access_nbtsearch_Position_drop(&self->current);
		self->has_current = false;
	}
	else
	{
		access_nbtsearch_Position_Clone_clone(&self->current, &self->resume_after);
		self->has_resume_after = true;
	}
	*result = restart;
	return true;
}

/*
 * access_nbtsearch_BtScan_restrict starts the scan over within a unit of the
 * tree's keys, which it takes, as a worker of a parallel scan reads the unit
 * it took.
 */
void
access_nbtsearch_BtScan_restrict(AccessNbtsearchBtScan *self, AccessNbtsearchKeyRange *chunk)
{
	if (self->has_chunk)
		access_nbtsearch_KeyRange_drop(&self->chunk);
	self->chunk = *chunk;
	self->has_chunk = true;
	self->searching = false;
	self->need_prim_scan = false;
	self->started = false;
	self->ended = false;
	if (self->has_current)
		access_nbtsearch_Position_drop(&self->current);
	self->has_current = false;
	if (self->has_resume_after)
		access_nbtsearch_Position_drop(&self->resume_after);
	self->has_resume_after = false;
}

/* access_nbtsearch_BtScan_nsearches returns how many searches of the index the scan began, Postgres' nsearches. */
uint64
access_nbtsearch_BtScan_nsearches(const AccessNbtsearchBtScan *self)
{
	return self->nsearches;
}

/*
 * access_nbtsearch_BtScan_markpos answers the position of the entry the scan
 * returned last, or false before its first, Postgres' btmarkpos.
 */
bool
access_nbtsearch_BtScan_markpos(const AccessNbtsearchBtScan *self, AccessNbtsearchBtMark *result)
{
	if (!self->has_current)
		return false;
	access_nbtsearch_Position_Clone_clone(&self->current, &result->position);
	return true;
}

/*
 * access_nbtsearch_BtScan_restrpos moves the scan back to a marked position,
 * or to its start for none (NULL), so that it next returns the entry after
 * the marked one with its arrays back at their first elements, Postgres'
 * btrestrpos.
 */
void
access_nbtsearch_BtScan_restrpos(AccessNbtsearchBtScan *self, AccessNbtsearchSearchCalls calls,
								 const AccessNbtsearchBtMark *mark)
{
	self->searching = false;
	self->need_prim_scan = false;
	self->ended = false;
	if (self->has_resume_after)
		access_nbtsearch_Position_drop(&self->resume_after);
	if (self->has_current)
		access_nbtsearch_Position_drop(&self->current);
	self->has_resume_after = self->has_current = false;
	if (mark != NULL)
	{
		if (self->pre.arrays.len != 0)
		{
			AccessNbtutilsChecker checker = access_nbtsearch_BtScan_checker(self, calls);

			access_nbtutils_Checker_start_array_keys(&checker, access_nbtsearch_BtScan_dir(self));
		}
		self->started = true;
		access_nbtsearch_Position_Clone_clone(&mark->position, &self->resume_after);
		access_nbtsearch_Position_Clone_clone(&mark->position, &self->current);
		self->has_resume_after = self->has_current = true;
	}
	else
		self->started = false;
}

/* access_nbtsearch_BtMark_Clone_clone copies a mark and its position. */
void
access_nbtsearch_BtMark_Clone_clone(const AccessNbtsearchBtMark *self, AccessNbtsearchBtMark *result)
{
	access_nbtsearch_Position_Clone_clone(&self->position, &result->position);
}

/* access_nbtsearch_BtMark_drop frees a mark's position. */
void
access_nbtsearch_BtMark_drop(AccessNbtsearchBtMark *self)
{
	access_nbtsearch_Position_drop(&self->position);
}
#endif
