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
 * The C form of std's HashMap (the port text's section 9): a port of
 * hashbrown 0.17.1's RawTable as std 1.99.1 builds it, with the group
 * width the target's hashbrown uses (SSE2's 16 on x86-64, NEON's 8 on
 * ARM64, the generic word's 8 elsewhere), power-of-two buckets, the 7/8
 * load factor, erase's choice between EMPTY and DELETED, and
 * reserve_rehash's choice between rehashing in place and growing, so its
 * allocations and frees fall where std's do; its scope guards are landing
 * pads. The elements end at the control bytes, the first element just below
 * them. The allocator gives MAXALIGN, so the groups load unaligned. std is
 * built without overflow checks, so its arithmetic is plain C's.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "port/pg_bitutils.h"
#include "port/pg_bswap.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/hashmap.h"
#include "doltgres/std/unwind.h"

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define STD_GROUP_SSE2
#elif (defined(__aarch64__) || defined(_M_ARM64)) && defined(__ARM_NEON) && !defined(WORDS_BIGENDIAN)
#include <arm_neon.h>
#define STD_GROUP_NEON
#endif

/* STD_HASHBROWN_RAW_FILE is hashbrown's raw.rs as std's build names it, where a capacity overflow panics. */
#define STD_HASHBROWN_RAW_FILE "/rust/deps" STD_SEP "hashbrown-0.17.1" STD_SEP "src" STD_SEP "raw.rs"

/* STD_TAG_EMPTY and STD_TAG_DELETED are the control bytes of an empty and a deleted bucket. */
#define STD_TAG_EMPTY 0xFF
#define STD_TAG_DELETED 0x80

#if defined(STD_GROUP_SSE2)

/* STD_GROUP_WIDTH is the number of control bytes a group covers. */
#define STD_GROUP_WIDTH 16

/* STD_BITMASK_STRIDE is the bits of a bitmask each control byte takes. */
#define STD_BITMASK_STRIDE 1

/* STD_BITMASK_ITER_MASK is the bits of a bitmask an iteration over it walks. */
#define STD_BITMASK_ITER_MASK UINT64CONST(0xFFFF)

/* STD_BITMASK_BITS is the bits of a bitmask's word. */
#define STD_BITMASK_BITS 16

/* std_Group_match_tag returns the bitmask of a group's bytes equal to a tag. */
static inline uint64
std_Group_match_tag(const uint8 *ctrl, uint8 tag)
{
	__m128i		group = _mm_loadu_si128((const __m128i *) ctrl);
	__m128i		cmp = _mm_cmpeq_epi8(group, _mm_set1_epi8((char) tag));

	return (uint16) _mm_movemask_epi8(cmp);
}

/* std_Group_match_empty_or_deleted returns the bitmask of a group's special bytes, their high bits. */
static inline uint64
std_Group_match_empty_or_deleted(const uint8 *ctrl)
{
	__m128i		group = _mm_loadu_si128((const __m128i *) ctrl);

	return (uint16) _mm_movemask_epi8(group);
}

/* std_Group_match_full returns the bitmask of a group's full bytes. */
static inline uint64
std_Group_match_full(const uint8 *ctrl)
{
	return (uint16) ~std_Group_match_empty_or_deleted(ctrl);
}

/* std_Group_convert_special_to_empty_and_full_to_deleted rewrites a group: special bytes EMPTY, full ones DELETED. */
static inline void
std_Group_convert_special_to_empty_and_full_to_deleted(uint8 *ctrl)
{
	__m128i		group = _mm_loadu_si128((const __m128i *) ctrl);
	__m128i		zero = _mm_setzero_si128();
	__m128i		special = _mm_cmpgt_epi8(zero, group);

	_mm_storeu_si128((__m128i *) ctrl, _mm_or_si128(special, _mm_set1_epi8((char) STD_TAG_DELETED)));
}

#elif defined(STD_GROUP_NEON)

#define STD_GROUP_WIDTH 8
#define STD_BITMASK_STRIDE 8
#define STD_BITMASK_ITER_MASK UINT64CONST(0x8080808080808080)
#define STD_BITMASK_BITS 64

/* std_Group_match_tag returns the bitmask of a group's bytes equal to a tag. */
static inline uint64
std_Group_match_tag(const uint8 *ctrl, uint8 tag)
{
	uint8x8_t	cmp = vceq_u8(vld1_u8(ctrl), vdup_n_u8(tag));

	return vget_lane_u64(vreinterpret_u64_u8(cmp), 0);
}

/* std_Group_match_empty_or_deleted returns the bitmask of a group's special bytes. */
static inline uint64
std_Group_match_empty_or_deleted(const uint8 *ctrl)
{
	uint8x8_t	cmp = vcltz_s8(vreinterpret_s8_u8(vld1_u8(ctrl)));

	return vget_lane_u64(vreinterpret_u64_u8(cmp), 0);
}

/* std_Group_match_full returns the bitmask of a group's full bytes. */
static inline uint64
std_Group_match_full(const uint8 *ctrl)
{
	uint8x8_t	cmp = vcgez_s8(vreinterpret_s8_u8(vld1_u8(ctrl)));

	return vget_lane_u64(vreinterpret_u64_u8(cmp), 0);
}

/* std_Group_convert_special_to_empty_and_full_to_deleted rewrites a group: special bytes EMPTY, full ones DELETED. */
static inline void
std_Group_convert_special_to_empty_and_full_to_deleted(uint8 *ctrl)
{
	uint8x8_t	special = vcltz_s8(vreinterpret_s8_u8(vld1_u8(ctrl)));

	vst1_u8(ctrl, vorr_u8(special, vdup_n_u8(STD_TAG_DELETED)));
}

#else

#define STD_GROUP_WIDTH 8
#define STD_BITMASK_STRIDE 8
#define STD_BITMASK_ITER_MASK PG_UINT64_MAX
#define STD_BITMASK_BITS 64

/* STD_GROUP_REPEAT is a byte repeated in each of a word's bytes. */
#define STD_GROUP_REPEAT(b) (UINT64CONST(0x0101010101010101) * (uint8) (b))

/* std_group_load reads a group's word, its first byte the low one. */
static inline uint64
std_group_load(const uint8 *ctrl)
{
	uint64		word;

	memcpy(&word, ctrl, 8);
#ifdef WORDS_BIGENDIAN
	word = pg_bswap64(word);
#endif
	return word;
}

/* std_Group_match_tag returns the bitmask of a group's bytes that may equal a tag, as the generic group's does. */
static inline uint64
std_Group_match_tag(const uint8 *ctrl, uint8 tag)
{
	uint64		cmp = std_group_load(ctrl) ^ STD_GROUP_REPEAT(tag);

	return (cmp - STD_GROUP_REPEAT(0x01)) & ~cmp & STD_GROUP_REPEAT(STD_TAG_DELETED);
}

/* std_Group_match_empty_or_deleted returns the bitmask of a group's special bytes. */
static inline uint64
std_Group_match_empty_or_deleted(const uint8 *ctrl)
{
	return std_group_load(ctrl) & STD_GROUP_REPEAT(STD_TAG_DELETED);
}

/* std_Group_match_full returns the bitmask of a group's full bytes. */
static inline uint64
std_Group_match_full(const uint8 *ctrl)
{
	return std_Group_match_empty_or_deleted(ctrl) ^ STD_GROUP_REPEAT(STD_TAG_DELETED);
}

/* std_Group_convert_special_to_empty_and_full_to_deleted rewrites a group: special bytes EMPTY, full ones DELETED. */
static inline void
std_Group_convert_special_to_empty_and_full_to_deleted(uint8 *ctrl)
{
	uint64		word = std_group_load(ctrl);
	uint64		full = ~word & STD_GROUP_REPEAT(STD_TAG_DELETED);

	word = ~full + (full >> 7);
#ifdef WORDS_BIGENDIAN
	word = pg_bswap64(word);
#endif
	memcpy(ctrl, &word, 8);
}

#endif

/* std_Group_match_empty returns the bitmask of a group's empty bytes. */
static inline uint64
std_Group_match_empty(const uint8 *ctrl)
{
	return std_Group_match_tag(ctrl, STD_TAG_EMPTY);
}

/* std_BitMask_lowest_set_bit returns the index of a bitmask's lowest set byte, false when it has none. */
static inline bool
std_BitMask_lowest_set_bit(uint64 mask, size_t *result)
{
	if (mask == 0)
		return false;
	*result = (size_t) pg_rightmost_one_pos64(mask) / STD_BITMASK_STRIDE;
	return true;
}

/* std_BitMask_trailing_zeros returns the count of a bitmask's clear bytes below its lowest set one. */
static inline size_t
std_BitMask_trailing_zeros(uint64 mask)
{
	if (mask == 0)
		return STD_BITMASK_BITS / STD_BITMASK_STRIDE;
	return (size_t) pg_rightmost_one_pos64(mask) / STD_BITMASK_STRIDE;
}

/* std_BitMask_leading_zeros returns the count of a bitmask's clear bytes above its highest set one. */
static inline size_t
std_BitMask_leading_zeros(uint64 mask)
{
	if (mask == 0)
		return STD_BITMASK_BITS / STD_BITMASK_STRIDE;
	return (size_t) (STD_BITMASK_BITS - 1 - pg_leftmost_one_pos64(mask)) / STD_BITMASK_STRIDE;
}

/* std_Tag_full is Tag::full: a full bucket's control byte, the hash's top seven bits. */
static inline uint8
std_Tag_full(uint64 hash)
{
	return (uint8) ((hash >> (64 - 7)) & 0x7f);
}

/* std_Tag_is_full reports whether a control byte is a full bucket's. */
static inline bool
std_Tag_is_full(uint8 tag)
{
	return (tag & 0x80) == 0;
}

/* std_Tag_special_is_empty reports whether a special control byte is an empty bucket's. */
static inline bool
std_Tag_special_is_empty(uint8 tag)
{
	return (tag & 0x01) != 0;
}

/* STD_STATIC_EMPTY is the control bytes of a table with no buckets: a group of EMPTY. */
static pg_attribute_aligned(STD_GROUP_WIDTH)
const uint8 STD_STATIC_EMPTY[STD_GROUP_WIDTH] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
#if STD_GROUP_WIDTH == 16
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
#endif
};

/* StdProbeSeq is a probe sequence: the group's position and the stride so far. */
typedef struct StdProbeSeq
{
	size_t		pos;
	size_t		stride;
} StdProbeSeq;

/* std_ProbeSeq_move_next moves to the sequence's next group, a stride a group longer each time. */
static inline void
std_ProbeSeq_move_next(StdProbeSeq *self, size_t bucket_mask)
{
	self->stride += STD_GROUP_WIDTH;
	self->pos += self->stride;
	self->pos &= bucket_mask;
}

/* std_capacity_overflow panics as an infallible reserve does at a capacity overflow. */
pg_noreturn static void
std_capacity_overflow(void)
{
	static const char text[] = "Hash table capacity overflow";

	std_panic(STD_HASHBROWN_RAW_FILE, 36, 40, text, sizeof(text) - 1);
}

/* std_capacity_to_buckets returns the buckets a capacity needs, false on overflow. */
static bool
std_capacity_to_buckets(size_t cap, size_t size, size_t *result)
{
	size_t		adjusted_cap;

	if (cap < 15)
	{
		size_t		min_cap;

		if (STD_GROUP_WIDTH == 16 && size <= 1)
			min_cap = 14;
		else if ((STD_GROUP_WIDTH == 16 && size >= 2 && size <= 3) || (STD_GROUP_WIDTH == 8 && size <= 1))
			min_cap = 7;
		else
			min_cap = 3;
		cap = Max(min_cap, cap);
		if (cap < 4)
			*result = 4;
		else if (cap < 8)
			*result = 8;
		else
			*result = 16;
		return true;
	}
	if (cap > SIZE_MAX / 8)
		return false;
	adjusted_cap = cap * 8 / 7;
	*result = (size_t) pg_nextpower2_64(adjusted_cap);
	return true;
}

/* std_bucket_mask_to_capacity returns the elements a table of buckets holds: all of a small one's, 7/8 of a large one's. */
static inline size_t
std_bucket_mask_to_capacity(size_t bucket_mask)
{
	if (bucket_mask < 8)
		return bucket_mask;
	else
		return ((bucket_mask + 1) / 8) * 7;
}

/* std_ctrl_align is TableLayout's ctrl_align: the element's alignment, at least a group's width. */
static inline size_t
std_ctrl_align(const StdHashMapType *t)
{
	return t->align > STD_GROUP_WIDTH ? t->align : STD_GROUP_WIDTH;
}

/* std_calculate_layout_for returns a table's allocation size and where its control bytes start, false on overflow. */
static bool
std_calculate_layout_for(const StdHashMapType *t, size_t buckets, size_t *len, size_t *ctrl_offset)
{
	size_t		ctrl_align = std_ctrl_align(t);

	if (t->size != 0 && buckets > SIZE_MAX / t->size)
		return false;
	if (t->size * buckets > SIZE_MAX - (ctrl_align - 1))
		return false;
	*ctrl_offset = (t->size * buckets + (ctrl_align - 1)) & ~(ctrl_align - 1);
	if (*ctrl_offset > SIZE_MAX - (buckets + STD_GROUP_WIDTH))
		return false;
	*len = *ctrl_offset + buckets + STD_GROUP_WIDTH;
	if (*len > (size_t) PG_INT64_MAX - (ctrl_align - 1))
		return false;
	return true;
}

/* std_RawTableInner_NEW is RawTableInner::NEW: no buckets, its control bytes the static group of EMPTY. */
static StdRawTableInner
std_RawTableInner_NEW(void)
{
	StdRawTableInner table = {(uint8 *) STD_STATIC_EMPTY, 0, 0, 0};

	return table;
}

/* std_RawTableInner_num_buckets is the table's buckets. */
static inline size_t
std_RawTableInner_num_buckets(const StdRawTableInner *self)
{
	return self->bucket_mask + 1;
}

/* std_RawTableInner_num_ctrl_bytes is the table's control bytes: one per bucket and a group's mirror. */
static inline size_t
std_RawTableInner_num_ctrl_bytes(const StdRawTableInner *self)
{
	return self->bucket_mask + 1 + STD_GROUP_WIDTH;
}

/* std_RawTableInner_is_empty_singleton reports whether the table has no buckets of its own. */
static inline bool
std_RawTableInner_is_empty_singleton(const StdRawTableInner *self)
{
	return self->bucket_mask == 0;
}

/* std_RawTableInner_bucket_ptr is an element's address: the indexth below the control bytes. */
static inline uint8 *
std_RawTableInner_bucket_ptr(const StdRawTableInner *self, size_t index, size_t size_of)
{
	return self->ctrl - (index + 1) * size_of;
}

/* std_RawTableInner_new_uninitialized allocates a table of buckets, its control bytes unset. */
static StdRawTableInner
std_RawTableInner_new_uninitialized(const StdHashMapType *t, size_t buckets)
{
	size_t		len;
	size_t		ctrl_offset;
	uint8	   *ptr;
	StdRawTableInner table;

	if (!std_calculate_layout_for(t, buckets, &len, &ctrl_offset))
		std_capacity_overflow();
	ptr = std_alloc(len, std_ctrl_align(t));
	table.ctrl = ptr + ctrl_offset;
	table.bucket_mask = buckets - 1;
	table.items = 0;
	table.growth_left = std_bucket_mask_to_capacity(buckets - 1);
	return table;
}

/* std_RawTableInner_with_capacity returns an empty table that holds a capacity, the static one for none. */
static StdRawTableInner
std_RawTableInner_with_capacity(const StdHashMapType *t, size_t capacity)
{
	size_t		buckets;
	StdRawTableInner result;

	if (capacity == 0)
		return std_RawTableInner_NEW();
	if (!std_capacity_to_buckets(capacity, t->size, &buckets))
		std_capacity_overflow();
	result = std_RawTableInner_new_uninitialized(t, buckets);
	memset(result.ctrl, STD_TAG_EMPTY, std_RawTableInner_num_ctrl_bytes(&result));
	return result;
}

static uint8 *std_RawTableInner_allocation_info(const StdRawTableInner *self, const StdHashMapType *t,
												size_t *len);

/* std_RawTableInner_free_buckets frees the table's allocation. */
static void
std_RawTableInner_free_buckets(StdRawTableInner *self, const StdHashMapType *t)
{
	uint8	   *ptr;
	size_t		len;

	ptr = std_RawTableInner_allocation_info(self, t, &len);
	std_free(ptr, len, std_ctrl_align(t));
}

/*
 * std_RawTableInner_allocation_info returns the table's allocation and its
 * size, as the table was allocated; the table must not be the empty
 * singleton.
 */
static uint8 *
std_RawTableInner_allocation_info(const StdRawTableInner *self, const StdHashMapType *t, size_t *len)
{
	size_t		ctrl_offset;

	if (!std_calculate_layout_for(t, std_RawTableInner_num_buckets(self), len, &ctrl_offset))
		pg_unreachable();
	return self->ctrl - ctrl_offset;
}

/* std_RawTableInner_probe_seq starts a hash's probe sequence at its h1's group. */
static inline StdProbeSeq
std_RawTableInner_probe_seq(const StdRawTableInner *self, uint64 hash)
{
	StdProbeSeq seq = {(size_t) hash & self->bucket_mask, 0};

	return seq;
}

/* std_RawTableInner_is_bucket_full reports whether a bucket holds an element. */
static inline bool
std_RawTableInner_is_bucket_full(const StdRawTableInner *self, size_t index)
{
	return std_Tag_is_full(self->ctrl[index]);
}

/* std_RawTableInner_fix_insert_index moves an index a small table's mirror gave to the first group's free bucket. */
static inline size_t
std_RawTableInner_fix_insert_index(const StdRawTableInner *self, size_t index)
{
	if (std_RawTableInner_is_bucket_full(self, index))
		(void) std_BitMask_lowest_set_bit(std_Group_match_empty_or_deleted(self->ctrl) & STD_BITMASK_ITER_MASK,
										  &index);
	return index;
}

/* std_RawTableInner_find_insert_index_in_group returns a group's first free bucket, false when it has none. */
static inline bool
std_RawTableInner_find_insert_index_in_group(const StdRawTableInner *self, uint64 empty_or_deleted,
											 const StdProbeSeq *probe_seq, size_t *result)
{
	size_t		bit;

	if (!std_BitMask_lowest_set_bit(empty_or_deleted, &bit))
		return false;
	*result = (probe_seq->pos + bit) & self->bucket_mask;
	return true;
}

/* std_RawTableInner_find_insert_index returns the first free bucket of a hash's probe sequence. */
static size_t
std_RawTableInner_find_insert_index(const StdRawTableInner *self, uint64 hash)
{
	StdProbeSeq probe_seq = std_RawTableInner_probe_seq(self, hash);

	for (;;)
	{
		size_t		index;

		if (std_RawTableInner_find_insert_index_in_group(self,
														 std_Group_match_empty_or_deleted(self->ctrl + probe_seq.pos),
														 &probe_seq, &index))
			return std_RawTableInner_fix_insert_index(self, index);
		std_ProbeSeq_move_next(&probe_seq, self->bucket_mask);
	}
}

/* std_RawTableInner_set_ctrl sets a bucket's control byte and its mirror's. */
static inline void
std_RawTableInner_set_ctrl(StdRawTableInner *self, size_t index, uint8 ctrl)
{
	size_t		index2 = ((index - STD_GROUP_WIDTH) & self->bucket_mask) + STD_GROUP_WIDTH;

	self->ctrl[index] = ctrl;
	self->ctrl[index2] = ctrl;
}

/* std_RawTableInner_set_ctrl_hash marks a bucket full with a hash's tag. */
static inline void
std_RawTableInner_set_ctrl_hash(StdRawTableInner *self, size_t index, uint64 hash)
{
	std_RawTableInner_set_ctrl(self, index, std_Tag_full(hash));
}

/* std_RawTableInner_replace_ctrl_hash marks a bucket full with a hash's tag and returns its control byte before. */
static inline uint8
std_RawTableInner_replace_ctrl_hash(StdRawTableInner *self, size_t index, uint64 hash)
{
	uint8		prev_ctrl = self->ctrl[index];

	std_RawTableInner_set_ctrl_hash(self, index, hash);
	return prev_ctrl;
}

/* std_RawTableInner_prepare_insert_index finds a hash's free bucket and marks it full, returning its control byte before. */
static size_t
std_RawTableInner_prepare_insert_index(StdRawTableInner *self, uint64 hash, uint8 *old_ctrl)
{
	size_t		index = std_RawTableInner_find_insert_index(self, hash);

	*old_ctrl = self->ctrl[index];
	std_RawTableInner_set_ctrl_hash(self, index, hash);
	return index;
}

/* std_RawTableInner_record_item_insert_at counts an element put in a bucket of a control byte. */
static inline void
std_RawTableInner_record_item_insert_at(StdRawTableInner *self, size_t index, uint8 old_ctrl, uint8 new_ctrl)
{
	self->growth_left -= std_Tag_special_is_empty(old_ctrl) ? 1 : 0;
	std_RawTableInner_set_ctrl(self, index, new_ctrl);
	self->items += 1;
}

/* std_RawTableInner_is_in_same_group reports whether two buckets fall in one group of a hash's probe sequence. */
static inline bool
std_RawTableInner_is_in_same_group(const StdRawTableInner *self, size_t i, size_t new_i, uint64 hash)
{
	size_t		probe_seq_pos = std_RawTableInner_probe_seq(self, hash).pos;

	return ((i - probe_seq_pos) & self->bucket_mask) / STD_GROUP_WIDTH ==
		((new_i - probe_seq_pos) & self->bucket_mask) / STD_GROUP_WIDTH;
}

/* std_hash_elem returns the hash of an element's key under the map's BuildHasher. */
static uint64
std_hash_elem(const StdHashMap *map, const StdHashMapType *t, const void *key)
{
	return std_BuildHasher_hash_one(t->hasher, &map->hash_builder, key, t->key_hash);
}

/* std_drop_elem drops an element: its key, then its value. */
static void
std_drop_elem(const StdHashMapType *t, uint8 *elem)
{
	if (t->key_drop != NULL)
		t->key_drop(elem);
	if (t->value_drop != NULL)
		t->value_drop(elem + t->value_offset);
}

/* std_RawTableInner_find_inner returns the bucket of a key in a hash's probe sequence, false when none holds it. */
static bool
std_RawTableInner_find_inner(const StdRawTableInner *self, const StdHashMapType *t, uint64 hash,
							 const void *key, size_t *result)
{
	uint8		tag_hash = std_Tag_full(hash);
	StdProbeSeq probe_seq = std_RawTableInner_probe_seq(self, hash);

	for (;;)
	{
		const uint8 *group = self->ctrl + probe_seq.pos;
		uint64		bits = std_Group_match_tag(group, tag_hash) & STD_BITMASK_ITER_MASK;
		size_t		bit;

		while (std_BitMask_lowest_set_bit(bits, &bit))
		{
			size_t		index = (probe_seq.pos + bit) & self->bucket_mask;

			if (t->key_eq(key, std_RawTableInner_bucket_ptr(self, index, t->size)))
			{
				*result = index;
				return true;
			}
			bits &= bits - 1;
		}
		if (std_Group_match_empty(group) != 0)
			return false;
		std_ProbeSeq_move_next(&probe_seq, self->bucket_mask);
	}
}

/*
 * std_RawTableInner_find_or_find_insert_index_inner returns the bucket of
 * a key in a hash's probe sequence with true, or with false the first free
 * bucket the sequence passed.
 */
static bool
std_RawTableInner_find_or_find_insert_index_inner(const StdRawTableInner *self, const StdHashMapType *t,
												  uint64 hash, const void *key, size_t *result)
{
	bool		has_insert_index = false;
	size_t		insert_index = 0;
	uint8		tag_hash = std_Tag_full(hash);
	StdProbeSeq probe_seq = std_RawTableInner_probe_seq(self, hash);

	for (;;)
	{
		const uint8 *group = self->ctrl + probe_seq.pos;
		uint64		bits = std_Group_match_tag(group, tag_hash) & STD_BITMASK_ITER_MASK;
		size_t		bit;

		while (std_BitMask_lowest_set_bit(bits, &bit))
		{
			size_t		index = (probe_seq.pos + bit) & self->bucket_mask;

			if (t->key_eq(key, std_RawTableInner_bucket_ptr(self, index, t->size)))
			{
				*result = index;
				return true;
			}
			bits &= bits - 1;
		}
		if (!has_insert_index)
			has_insert_index = std_RawTableInner_find_insert_index_in_group(self,
																			std_Group_match_empty_or_deleted(group),
																			&probe_seq, &insert_index);
		if (has_insert_index && std_Group_match_empty(group) != 0)
		{
			*result = std_RawTableInner_fix_insert_index(self, insert_index);
			return false;
		}
		std_ProbeSeq_move_next(&probe_seq, self->bucket_mask);
	}
}

/* std_RawTableInner_prepare_rehash_in_place marks every full bucket DELETED and every special one EMPTY, mirror included. */
static void
std_RawTableInner_prepare_rehash_in_place(StdRawTableInner *self)
{
	size_t		buckets = std_RawTableInner_num_buckets(self);

	for (size_t i = 0; i < buckets; i += STD_GROUP_WIDTH)
		std_Group_convert_special_to_empty_and_full_to_deleted(self->ctrl + i);
	if (buckets < STD_GROUP_WIDTH)
		memmove(self->ctrl + STD_GROUP_WIDTH, self->ctrl, buckets);
	else
		memmove(self->ctrl + buckets, self->ctrl, STD_GROUP_WIDTH);
}

/* std_RawTableInner_drop_elements drops every element of the table, in its iteration's order. */
static void
std_RawTableInner_drop_elements(StdRawTableInner *self, const StdHashMapType *t)
{
	size_t		items = self->items;

	if ((t->key_drop == NULL && t->value_drop == NULL) || items == 0)
		return;
	for (size_t base = 0; items > 0; base += STD_GROUP_WIDTH)
	{
		uint64		bits = std_Group_match_full(self->ctrl + base) & STD_BITMASK_ITER_MASK;
		size_t		bit;

		while (std_BitMask_lowest_set_bit(bits, &bit))
		{
			std_drop_elem(t, std_RawTableInner_bucket_ptr(self, base + bit, t->size));
			items--;
			bits &= bits - 1;
		}
	}
}

/* std_RawTableInner_drop_inner_table drops the table's elements and frees it, unless it is the static one. */
static void
std_RawTableInner_drop_inner_table(StdRawTableInner *self, const StdHashMapType *t)
{
	if (!std_RawTableInner_is_empty_singleton(self))
	{
		std_RawTableInner_drop_elements(self, t);
		std_RawTableInner_free_buckets(self, t);
	}
}

/*
 * std_RawTableInner_resize_inner moves the elements to a new table that
 * holds a capacity, rehashing each, and frees the old one; should a hash
 * panic, the new table is freed.
 */
static void
std_RawTableInner_resize_inner(StdHashMap *map, const StdHashMapType *t, size_t capacity)
{
	StdRawTableInner *self = &map->table;
	volatile StdRawTableInner new_table = std_RawTableInner_with_capacity(t, capacity);
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);

	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		size_t		items = self->items;

		for (size_t base = 0; items > 0; base += STD_GROUP_WIDTH)
		{
			uint64		bits = std_Group_match_full(self->ctrl + base) & STD_BITMASK_ITER_MASK;
			size_t		bit;

			while (std_BitMask_lowest_set_bit(bits, &bit))
			{
				size_t		full_byte_index = base + bit;
				uint64		hash = std_hash_elem(map, t, std_RawTableInner_bucket_ptr(self, full_byte_index,
																					  t->size));
				uint8		old_ctrl;
				size_t		new_index = std_RawTableInner_prepare_insert_index((StdRawTableInner *) &new_table,
																			   hash, &old_ctrl);

				memcpy(std_RawTableInner_bucket_ptr((StdRawTableInner *) &new_table, new_index, t->size),
					   std_RawTableInner_bucket_ptr(self, full_byte_index, t->size), t->size);
				items--;
				bits &= bits - 1;
			}
		}
		std_unwind_pop(&unwind);
	}
	else
	{
		if (!std_RawTableInner_is_empty_singleton((StdRawTableInner *) &new_table))
			std_RawTableInner_free_buckets((StdRawTableInner *) &new_table, t);
		std_unwind_resume(&unwind);
	}
	new_table.growth_left -= self->items;
	new_table.items = self->items;
	{
		StdRawTableInner old = *self;

		*self = *(StdRawTableInner *) &new_table;
		if (!std_RawTableInner_is_empty_singleton(&old))
			std_RawTableInner_free_buckets(&old, t);
	}
}

/*
 * std_RawTableInner_rehash_in_place rehashes the elements within the
 * table's buckets, clearing its DELETED ones; should a hash panic, the
 * elements not yet placed are dropped.
 */
static void
std_RawTableInner_rehash_in_place(StdHashMap *map, const StdHashMapType *t)
{
	StdRawTableInner *self = &map->table;
	size_t		size_of = t->size;
	StdUnwind	unwind;

	std_RawTableInner_prepare_rehash_in_place(self);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		for (size_t i = 0; i < std_RawTableInner_num_buckets(self); i++)
		{
			uint8	   *i_p;

			if (self->ctrl[i] != STD_TAG_DELETED)
				continue;
			i_p = std_RawTableInner_bucket_ptr(self, i, size_of);
			for (;;)
			{
				uint64		hash = std_hash_elem(map, t, i_p);
				size_t		new_i = std_RawTableInner_find_insert_index(self, hash);
				uint8	   *new_i_p;
				uint8		prev_ctrl;

				if (std_RawTableInner_is_in_same_group(self, i, new_i, hash))
				{
					std_RawTableInner_set_ctrl_hash(self, i, hash);
					break;
				}
				new_i_p = std_RawTableInner_bucket_ptr(self, new_i, size_of);
				prev_ctrl = std_RawTableInner_replace_ctrl_hash(self, new_i, hash);
				if (prev_ctrl == STD_TAG_EMPTY)
				{
					std_RawTableInner_set_ctrl(self, i, STD_TAG_EMPTY);
					memcpy(new_i_p, i_p, size_of);
					break;
				}
				for (size_t k = 0; k < size_of; k++)
				{
					uint8		b = i_p[k];

					i_p[k] = new_i_p[k];
					new_i_p[k] = b;
				}
			}
		}
		std_unwind_pop(&unwind);
	}
	else
	{
		for (size_t i = 0; i < std_RawTableInner_num_buckets(self); i++)
		{
			if (self->ctrl[i] == STD_TAG_DELETED)
			{
				std_RawTableInner_set_ctrl(self, i, STD_TAG_EMPTY);
				std_drop_elem(t, std_RawTableInner_bucket_ptr(self, i, size_of));
				self->items -= 1;
			}
		}
		self->growth_left = std_bucket_mask_to_capacity(self->bucket_mask) - self->items;
		std_unwind_resume(&unwind);
	}
	self->growth_left = std_bucket_mask_to_capacity(self->bucket_mask) - self->items;
}

/* std_RawTableInner_clear_no_drop marks every bucket EMPTY without dropping the elements. */
static void
std_RawTableInner_clear_no_drop(StdRawTableInner *self)
{
	if (!std_RawTableInner_is_empty_singleton(self))
		memset(self->ctrl, STD_TAG_EMPTY, std_RawTableInner_num_ctrl_bytes(self));
	self->items = 0;
	self->growth_left = std_bucket_mask_to_capacity(self->bucket_mask);
}

/*
 * std_RawTableInner_erase marks a full bucket free: EMPTY when a probe
 * sequence through it can't have passed it full, DELETED otherwise.
 */
static void
std_RawTableInner_erase(StdRawTableInner *self, size_t index)
{
	size_t		index_before = (index - STD_GROUP_WIDTH) & self->bucket_mask;
	uint64		empty_before = std_Group_match_empty(self->ctrl + index_before);
	uint64		empty_after = std_Group_match_empty(self->ctrl + index);
	uint8		ctrl;

	if (std_BitMask_leading_zeros(empty_before) + std_BitMask_trailing_zeros(empty_after) >= STD_GROUP_WIDTH)
		ctrl = STD_TAG_DELETED;
	else
	{
		self->growth_left += 1;
		ctrl = STD_TAG_EMPTY;
	}
	std_RawTableInner_set_ctrl(self, index, ctrl);
	self->items -= 1;
}

/* std_RawTable_reserve_rehash makes room for more elements: a rehash in place when half the capacity is free, else growth. */
static void
std_RawTable_reserve_rehash(StdHashMap *map, const StdHashMapType *t, size_t additional)
{
	StdRawTableInner *self = &map->table;
	size_t		new_items;
	size_t		full_capacity;

	if (self->items > SIZE_MAX - additional)
		std_capacity_overflow();
	new_items = self->items + additional;
	full_capacity = std_bucket_mask_to_capacity(self->bucket_mask);
	if (new_items <= full_capacity / 2)
		std_RawTableInner_rehash_in_place(map, t);
	else
		std_RawTableInner_resize_inner(map, t, Max(new_items, full_capacity + 1));
}

/* std_RawTable_reserve makes room for more elements when the table's growth left is less. */
static void
std_RawTable_reserve(StdHashMap *map, const StdHashMapType *t, size_t additional)
{
	if (additional > map->table.growth_left)
		std_RawTable_reserve_rehash(map, t, additional);
}

/* std_RawTable_insert_at_index puts an element in a free bucket a hash's probe found. */
static void *
std_RawTable_insert_at_index(StdRawTableInner *self, const StdHashMapType *t, uint64 hash, size_t index,
							 const void *key, const void *value)
{
	uint8	   *bucket;

	std_RawTableInner_record_item_insert_at(self, index, self->ctrl[index], std_Tag_full(hash));
	bucket = std_RawTableInner_bucket_ptr(self, index, t->size);
	memcpy(bucket, key, t->value_offset);
	memcpy(bucket + t->value_offset, value, t->size - t->value_offset);
	return bucket;
}

/* std_HashMap_with_hasher is HashMap::with_hasher: an empty map of a BuildHasher. */
void
std_HashMap_with_hasher(StdHashMap *map, const StdHashBuilder *hash_builder)
{
	map->table = std_RawTableInner_NEW();
	map->hash_builder = *hash_builder;
}

/* std_HashMap_Default_default is HashMap's Default: an empty map of S's default. */
void
std_HashMap_Default_default(StdHashMap *map, const StdHashMapType *t)
{
	StdHashBuilder hash_builder;

	t->hasher_default(&hash_builder);
	std_HashMap_with_hasher(map, &hash_builder);
}

/* std_HashMap_len is the map's count of entries. */
size_t
std_HashMap_len(const StdHashMap *map)
{
	return map->table.items;
}

/* std_HashMap_is_empty reports whether the map holds no entry. */
bool
std_HashMap_is_empty(const StdHashMap *map)
{
	return map->table.items == 0;
}

/* std_HashMap_reserve makes room for more entries. */
void
std_HashMap_reserve(StdHashMap *map, const StdHashMapType *t, size_t additional)
{
	std_RawTable_reserve(map, t, additional);
}

/* std_HashMap_get returns a key's element, NULL when the map lacks it. */
void *
std_HashMap_get(const StdHashMap *map, const StdHashMapType *t, const void *key)
{
	size_t		index;

	if (!std_RawTableInner_find_inner(&map->table, t, std_hash_elem(map, t, key), key, &index))
		return NULL;
	return std_RawTableInner_bucket_ptr(&map->table, index, t->size);
}

/* std_HashMap_contains_key reports whether the map holds a key. */
bool
std_HashMap_contains_key(const StdHashMap *map, const StdHashMapType *t, const void *key)
{
	return std_HashMap_get(map, t, key) != NULL;
}

/*
 * std_HashMap_insert sets a key's value, taking both; when the map held
 * the key it returns true, its old value written to result, and drops the
 * key given. Should the table's growth panic, the key and value are
 * dropped.
 */
bool
std_HashMap_insert(StdHashMap *map, const StdHashMapType *t, void *key, void *value, void *result)
{
	uint64		hash;
	size_t		index;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);

	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		hash = std_hash_elem(map, t, key);
		std_RawTable_reserve(map, t, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (t->key_drop != NULL)
			t->key_drop(key);
		if (t->value_drop != NULL)
			t->value_drop(value);
		std_unwind_resume(&unwind);
	}
	if (std_RawTableInner_find_or_find_insert_index_inner(&map->table, t, hash, key, &index))
	{
		uint8	   *elem = std_RawTableInner_bucket_ptr(&map->table, index, t->size);
		size_t		value_size = t->size - t->value_offset;

		memcpy(result, elem + t->value_offset, value_size);
		memcpy(elem + t->value_offset, value, value_size);
		if (t->key_drop != NULL)
			t->key_drop(key);
		return true;
	}
	(void) std_RawTable_insert_at_index(&map->table, t, hash, index, key, value);
	return false;
}

/* std_HashMap_remove takes a key's entry out, returning true with its value written to result; the key is dropped. */
bool
std_HashMap_remove(StdHashMap *map, const StdHashMapType *t, const void *key, void *result)
{
	size_t		index;
	uint8	   *elem;

	if (!std_RawTableInner_find_inner(&map->table, t, std_hash_elem(map, t, key), key, &index))
		return false;
	std_RawTableInner_erase(&map->table, index);
	elem = std_RawTableInner_bucket_ptr(&map->table, index, t->size);
	memcpy(result, elem + t->value_offset, t->size - t->value_offset);
	if (t->key_drop != NULL)
		t->key_drop(elem);
	return true;
}

/* std_HashMap_entry is HashMap::entry: the key's element, or its hash with room made for one more entry. */
StdHashMapEntry
std_HashMap_entry(StdHashMap *map, const StdHashMapType *t, const void *key)
{
	StdHashMapEntry entry = {false, NULL, std_hash_elem(map, t, key)};
	size_t		index;

	if (std_RawTableInner_find_inner(&map->table, t, entry.hash, key, &index))
	{
		entry.occupied = true;
		entry.elem = std_RawTableInner_bucket_ptr(&map->table, index, t->size);
		return entry;
	}
	std_RawTable_reserve(map, t, 1);
	return entry;
}

/* std_HashMap_VacantEntry_insert puts a vacant entry's key and a value in the table, which has room, returning the element. */
void *
std_HashMap_VacantEntry_insert(StdHashMap *map, const StdHashMapType *t, const StdHashMapEntry *entry,
							   void *key, void *value)
{
	StdRawTableInner *self = &map->table;
	uint8		old_ctrl;
	size_t		index = std_RawTableInner_prepare_insert_index(self, entry->hash, &old_ctrl);
	uint8	   *bucket = std_RawTableInner_bucket_ptr(self, index, t->size);

	self->growth_left -= std_Tag_special_is_empty(old_ctrl) ? 1 : 0;
	memcpy(bucket, key, t->value_offset);
	memcpy(bucket + t->value_offset, value, t->size - t->value_offset);
	self->items += 1;
	return bucket;
}

/* std_HashMap_clear drops every entry, keeping the buckets; should a drop panic, the table is still cleared. */
void
std_HashMap_clear(StdHashMap *map, const StdHashMapType *t)
{
	StdUnwind	unwind;

	if (map->table.items == 0)
		return;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_RawTableInner_drop_elements(&map->table, t);
		std_unwind_pop(&unwind);
	}
	else
	{
		std_RawTableInner_clear_no_drop(&map->table);
		std_unwind_resume(&unwind);
	}
	std_RawTableInner_clear_no_drop(&map->table);
}

/*
 * std_HashMap_Iter_new is HashMap::iter, hashbrown's RawIter at the table's
 * first group: the elements in the table's order, group by group and each
 * group's full buckets lowest first.
 */
StdHashMapIter
std_HashMap_Iter_new(const StdHashMap *map)
{
	StdHashMapIter iter = {0};

	iter.items = map->table.items;
	if (iter.items > 0)
		iter.bits = std_Group_match_full(map->table.ctrl) & STD_BITMASK_ITER_MASK;
	return iter;
}

/* std_HashMap_Iter_next returns the iterator's next element, NULL past the last. */
void *
std_HashMap_Iter_next(StdHashMapIter *self, const StdRawTableInner *table, const StdHashMapType *t)
{
	size_t		bit;

	if (self->items == 0)
		return NULL;
	while (!std_BitMask_lowest_set_bit(self->bits, &bit))
	{
		self->base += STD_GROUP_WIDTH;
		self->bits = std_Group_match_full(table->ctrl + self->base) & STD_BITMASK_ITER_MASK;
	}
	self->bits &= self->bits - 1;
	self->items--;
	return std_RawTableInner_bucket_ptr(table, self->base + bit, t->size);
}

/*
 * std_HashMap_IntoIter_new is HashMap's IntoIterator, hashbrown's
 * RawIntoIter: the map's table, taken, and an iterator over it.
 */
StdHashMapIntoIter
std_HashMap_IntoIter_new(StdHashMap *map)
{
	StdHashMapIntoIter iter;

	iter.iter = std_HashMap_Iter_new(map);
	iter.table = map->table;
	map->table = std_RawTableInner_NEW();
	return iter;
}

/* std_HashMap_IntoIter_next moves the next element out to result, returning false past the last. */
bool
std_HashMap_IntoIter_next(StdHashMapIntoIter *self, const StdHashMapType *t, void *result)
{
	void	   *elem = std_HashMap_Iter_next(&self->iter, &self->table, t);

	if (elem == NULL)
		return false;
	memcpy(result, elem, t->size);
	return true;
}

/*
 * std_HashMap_IntoIter_drop is RawIntoIter's drop: the elements not taken
 * dropped, each value even when its key's drop panics, then the buckets
 * freed; a panicking drop leaks the rest and the buckets, as RawIntoIter's
 * drop has no guard.
 */
void
std_HashMap_IntoIter_drop(StdHashMapIntoIter *self, const StdHashMapType *t)
{
	void	   *elem;

	while ((elem = std_HashMap_Iter_next(&self->iter, &self->table, t)) != NULL)
	{
		StdUnwind	unwind;

		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
		{
			if (t->key_drop != NULL)
				t->key_drop(elem);
			std_unwind_pop(&unwind);
		}
		else
		{
			if (t->value_drop != NULL)
				t->value_drop((uint8 *) elem + t->value_offset);
			std_unwind_resume(&unwind);
		}
		if (t->value_drop != NULL)
			t->value_drop((uint8 *) elem + t->value_offset);
	}
	if (!std_RawTableInner_is_empty_singleton(&self->table))
		std_RawTableInner_free_buckets(&self->table, t);
}

/* std_HashMap_PartialEq_eq reports whether two maps hold the same keys with equal values. */
bool
std_HashMap_PartialEq_eq(const StdHashMap *map, const StdHashMap *other, const StdHashMapType *t,
						 bool (*value_eq) (const void *a, const void *b))
{
	size_t		items = map->table.items;
	size_t		n = 0;

	if (map->table.items != other->table.items)
		return false;
	for (size_t base = 0; n < items; base += STD_GROUP_WIDTH)
	{
		uint64		bits = std_Group_match_full(map->table.ctrl + base) & STD_BITMASK_ITER_MASK;
		size_t		bit;

		while (std_BitMask_lowest_set_bit(bits, &bit))
		{
			const uint8 *elem = std_RawTableInner_bucket_ptr(&map->table, base + bit, t->size);
			const uint8 *v = std_HashMap_get(other, t, elem);

			if (v == NULL || !value_eq(elem + t->value_offset, v + t->value_offset))
				return false;
			n++;
			bits &= bits - 1;
		}
	}
	return true;
}

/* std_HashMap_drop drops every entry and frees the table. */
void
std_HashMap_drop(StdHashMap *map, const StdHashMapType *t)
{
	std_RawTableInner_drop_inner_table(&map->table, t);
}
