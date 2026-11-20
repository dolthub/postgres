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
 * The delta's region of blocks (bugs row 116): every page's body is a fixed
 * slot of a block that holds pages of one size, its class; the budget counts
 * whole blocks taken; a block goes back to the free stack when its last page
 * is freed; and the region's one block past the budget is the compaction's
 * alone. A body is its place in the region plus one, so zero names none.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/delta.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/test.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#define RUST_FILE "host" STD_SEP "src" STD_SEP "delta" STD_SEP "blocks.rs"

/* The items below that an earlier item reaches, declared ahead so the file keeps blocks.rs's order. */
static bool host_delta_blocks_taken(HostDeltaDirectoryDirectory directory, uint32 class, bool overshooting,
									uint64 *result);
static bool host_delta_blocks_popped(HostDeltaDirectoryDirectory directory, uint32 *result);
static void host_delta_blocks_pushed(HostDeltaDirectoryDirectory directory, HostDeltaBlocksRegion list, uint32 block);
static void host_delta_blocks_unlinked(HostDeltaDirectoryDirectory directory, HostDeltaBlocksRegion list,
									   uint32 block);

/* host_delta_blocks_Region_index returns the word's place among the region's words. */
size_t
host_delta_blocks_Region_index(HostDeltaBlocksRegion self)
{
	switch (self.tag)
	{
		case HOST_DELTA_BLOCKS_REGION_FREE:
			return 0;
		case HOST_DELTA_BLOCKS_REGION_FRESH:
			return 1;
		case HOST_DELTA_BLOCKS_REGION_TAKEN:
			return 2;
		case HOST_DELTA_BLOCKS_REGION_OVER:
			return 3;
		case HOST_DELTA_BLOCKS_REGION_PARTIAL:
			break;
	}
	return STD_ADD(usize, 4, (size_t) self.u.partial, 51, 39);
}

/* A block holds whole slots of every class, the largest class is one block, and a block's slots fit its bitmap. */
StaticAssertDecl(HOST_DELTA_PAGE_LARGEST == HOST_DELTA_PAGE_BLOCK &&
				 HOST_DELTA_PAGE_SLOT_BYTES << (HOST_DELTA_BLOCKS_CLASSES - 1) == HOST_DELTA_PAGE_BLOCK &&
				 HOST_DELTA_PAGE_BLOCK / HOST_DELTA_PAGE_SLOT_BYTES <= 64,
				 "a block holds whole slots of every class");

/* The region's words that carry no class: Region::Free, Fresh, Taken, and Over. */
static const HostDeltaBlocksRegion HOST_DELTA_BLOCKS_FREE = {HOST_DELTA_BLOCKS_REGION_FREE};
static const HostDeltaBlocksRegion HOST_DELTA_BLOCKS_FRESH = {HOST_DELTA_BLOCKS_REGION_FRESH};
static const HostDeltaBlocksRegion HOST_DELTA_BLOCKS_TAKEN = {HOST_DELTA_BLOCKS_REGION_TAKEN};
static const HostDeltaBlocksRegion HOST_DELTA_BLOCKS_OVER = {HOST_DELTA_BLOCKS_REGION_OVER};

/* host_delta_blocks_class returns the class of a body's bytes: the power of two from 1 KiB that holds it. */
uint32
host_delta_blocks_class(uint64 body)
{
	uint64		power = std_next_power_of_two_u64(std_max_u64(body, HOST_DELTA_PAGE_SLOT_BYTES), STD_NUM_MOD_FILE,
												  1322, 5);

	return std_trailing_zeros_u64(STD_DIV_FOLDED(u64, power, HOST_DELTA_PAGE_SLOT_BYTES));
}

/* host_delta_blocks_bytes returns the bytes of a class's slot. */
static uint64
host_delta_blocks_bytes(uint32 class)
{
	return STD_SHL(u64, HOST_DELTA_PAGE_SLOT_BYTES, (uint64) class, 81, 5);
}

/* host_delta_blocks_full returns the bitmap of a block of a class whose every slot is taken. */
static uint64
host_delta_blocks_full(uint32 class)
{
	uint64		count = STD_DIV(u64, HOST_DELTA_PAGE_BLOCK, host_delta_blocks_bytes(class), 86, 23);

	return STD_SHR(u64, PG_UINT64_MAX, STD_SUB(u64, 64, count, 86, 17), 86, 5);
}

/*
 * host_delta_blocks_budgeted returns the budget's blocks: every block of the
 * region but the one past the budget, so a give, which runs at a process's
 * exit, reads no setting.
 */
static uint64
host_delta_blocks_budgeted(HostDeltaDirectoryDirectory directory)
{
	return STD_SUB(u64, (uint64) directory.ops->blocks(directory.self), 1, 92, 5);
}

/* host_delta_blocks_used returns the bytes the region's taken blocks hold. */
uint64
host_delta_blocks_used(HostDeltaDirectoryDirectory directory)
{
	return STD_MUL(u64, directory.ops->region(directory.self, HOST_DELTA_BLOCKS_TAKEN),
				   HOST_DELTA_PAGE_BLOCK, 97, 5);
}

/*
 * host_delta_blocks_take takes a zeroed body of a page's size: a free slot of
 * a block of its class, or else a free block while the blocks taken are under
 * the budget's. The one compaction past the budget (overshooting) may also
 * take a free slot of the block past the budget, or that block itself while
 * no take holds it. It answers nothing when it refuses, which happens only
 * with the budget's every block taken.
 */
bool
host_delta_blocks_take(HostDeltaDirectoryDirectory directory, HostDeltaPageSize size, bool overshooting,
					   uint64 *result)
{
	uint32		class = host_delta_blocks_class(host_delta_page_Size_body(size));
	bool		has_taken;
	uint64		body;

	directory.ops->lock_region(directory.self);
	has_taken = host_delta_blocks_taken(directory, class, overshooting, &body);
	directory.ops->unlock_region(directory.self);
	if (!has_taken)
		return false;
	directory.ops->zero(directory.self, body, size);
	*result = body;
	return true;
}

/* host_delta_blocks_taken takes a slot of a class under the region's lock. */
static bool
host_delta_blocks_taken(HostDeltaDirectoryDirectory directory, uint32 class, bool overshooting, uint64 *result)
{
	HostDeltaBlocksRegion partial = {HOST_DELTA_BLOCKS_REGION_PARTIAL, {class}};
	uint64		over_word = directory.ops->region(directory.self, HOST_DELTA_BLOCKS_OVER);
	bool		has_over = over_word != 0;
	uint32		over = has_over ? (uint32) (over_word - 1) : 0;
	uint64		partial_word = directory.ops->region(directory.self, partial);
	uint32		block;
	uint64		map;
	bool		found = false;
	uint32		slot;

	if (partial_word != 0)
	{
		block = (uint32) (partial_word - 1);
		map = directory.ops->block(directory.self, block, HOST_DELTA_BLOCKS_BLOCK_MAP);
		found = true;
	}
	else if (has_over && overshooting)
	{
		uint64		fits_map = directory.ops->block(directory.self, over, HOST_DELTA_BLOCKS_BLOCK_MAP);

		if (directory.ops->block(directory.self, over, HOST_DELTA_BLOCKS_BLOCK_SLOTS) == (uint64) class &&
			fits_map != host_delta_blocks_full(class))
		{
			block = over;
			map = fits_map;
			found = true;
		}
	}
	if (!found)
	{
		uint64		taken = directory.ops->region(directory.self, HOST_DELTA_BLOCKS_TAKEN);

		if (overshooting)
		{
			if (has_over)
				return false;
		}
		else
		{
			uint64		tmp_1 = STD_SUB(u64, taken, has_over ? 1 : 0, 130, 30);

			if (tmp_1 >= host_delta_blocks_budgeted(directory))
				return false;
		}
		if (!host_delta_blocks_popped(directory, &block))
			return false;
		directory.ops->set_region(directory.self, HOST_DELTA_BLOCKS_TAKEN,
								  STD_ADD(u64, taken, 1, 134, 53));
		if (overshooting)
			directory.ops->set_region(directory.self, HOST_DELTA_BLOCKS_OVER,
									  STD_ADD(u64, (uint64) block, 1, 136, 64));
		else if (host_delta_blocks_full(class) != 1)
			host_delta_blocks_pushed(directory, partial, block);
		map = 0;
	}
	slot = std_trailing_zeros_u64(~map);
	map = map | STD_SHL(u64, 1, (uint64) slot, 145, 21);
	if (map == host_delta_blocks_full(class) && host_delta_blocks_full(class) != 1 && !(has_over && block == over))
		host_delta_blocks_unlinked(directory, partial, block);
	directory.ops->set_block(directory.self, block, HOST_DELTA_BLOCKS_BLOCK_SLOTS, (uint64) class);
	directory.ops->set_block(directory.self, block, HOST_DELTA_BLOCKS_BLOCK_MAP, map);
	{
		uint64		tmp_2 = STD_MUL(u64, (uint64) block, HOST_DELTA_PAGE_BLOCK, 151, 10);
		uint64		tmp_3 = STD_MUL(u64, (uint64) slot, host_delta_blocks_bytes(class), 151, 37);

		*result = STD_ADD(u64, STD_ADD(u64, tmp_2, tmp_3, 151, 10), 1, 151, 10);
	}
	return true;
}

/*
 * host_delta_blocks_give gives a body back to its block, and the block to the
 * free stack when it held its last page. Once the blocks taken are back at the
 * budget's, the block past it serves the budget, its class's list taking it
 * while it has a free slot, so a take past the budget may take another.
 */
void
host_delta_blocks_give(HostDeltaDirectoryDirectory directory, uint64 body)
{
	uint64		at = STD_SUB(u64, body, 1, 158, 14);
	uint32		block = (uint32) STD_DIV_FOLDED(u64, at, HOST_DELTA_PAGE_BLOCK);
	uint64		over_word;
	bool		has_over;
	uint32		over;
	uint32		class;
	uint64		taken;
	uint64		map;
	bool		listed;
	HostDeltaBlocksRegion partial;

	directory.ops->lock_region(directory.self);
	over_word = directory.ops->region(directory.self, HOST_DELTA_BLOCKS_OVER);
	has_over = over_word != 0;
	over = has_over ? (uint32) (over_word - 1) : 0;
	class = (uint32) directory.ops->block(directory.self, block, HOST_DELTA_BLOCKS_BLOCK_SLOTS);
	taken = directory.ops->block(directory.self, block, HOST_DELTA_BLOCKS_BLOCK_MAP);
	partial.tag = HOST_DELTA_BLOCKS_REGION_PARTIAL;
	partial.u.partial = class;
	map = taken &
		~STD_SHL(u64, 1, STD_DIV(u64, STD_REM_FOLDED(u64, at, HOST_DELTA_PAGE_BLOCK), host_delta_blocks_bytes(class),
								 164, 30), 164, 24);
	listed = taken != host_delta_blocks_full(class) && !(has_over && block == over);
	if (map == 0)
	{
		if (listed)
			host_delta_blocks_unlinked(directory, partial, block);
		if (has_over && block == over)
			directory.ops->set_region(directory.self, HOST_DELTA_BLOCKS_OVER, 0);
		directory.ops->set_block(directory.self, block, HOST_DELTA_BLOCKS_BLOCK_SLOTS, 0);
		directory.ops->set_block(directory.self, block, HOST_DELTA_BLOCKS_BLOCK_MAP, 0);
		directory.ops->set_block(directory.self, block, HOST_DELTA_BLOCKS_BLOCK_LINKS,
								 directory.ops->region(directory.self, HOST_DELTA_BLOCKS_FREE));
		directory.ops->set_region(directory.self, HOST_DELTA_BLOCKS_FREE,
								  STD_ADD(u64, (uint64) block, 1, 176, 44));
		directory.ops->set_region(directory.self, HOST_DELTA_BLOCKS_TAKEN,
								  STD_SUB(u64, directory.ops->region(directory.self,
																	 HOST_DELTA_BLOCKS_TAKEN),
										  1, 177, 45));
	}
	else
	{
		directory.ops->set_block(directory.self, block, HOST_DELTA_BLOCKS_BLOCK_MAP, map);
		if (!listed && !(has_over && block == over))
			host_delta_blocks_pushed(directory, partial, block);
	}
	over_word = directory.ops->region(directory.self, HOST_DELTA_BLOCKS_OVER);
	if (over_word != 0 &&
		directory.ops->region(directory.self, HOST_DELTA_BLOCKS_TAKEN) <=
		host_delta_blocks_budgeted(directory))
	{
		uint32		over_block = (uint32) (over_word - 1);
		uint32		over_class;
		HostDeltaBlocksRegion over_partial;

		directory.ops->set_region(directory.self, HOST_DELTA_BLOCKS_OVER, 0);
		over_class = (uint32) directory.ops->block(directory.self, over_block, HOST_DELTA_BLOCKS_BLOCK_SLOTS);
		if (directory.ops->block(directory.self, over_block, HOST_DELTA_BLOCKS_BLOCK_MAP) !=
			host_delta_blocks_full(over_class))
		{
			over_partial.tag = HOST_DELTA_BLOCKS_REGION_PARTIAL;
			over_partial.u.partial = over_class;
			host_delta_blocks_pushed(directory, over_partial, over_block);
		}
	}
	directory.ops->unlock_region(directory.self);
}

/* host_delta_blocks_popped takes a block off the free stack, or else the region's next fresh block. */
static bool
host_delta_blocks_popped(HostDeltaDirectoryDirectory directory, uint32 *result)
{
	uint64		free_word = directory.ops->region(directory.self, HOST_DELTA_BLOCKS_FREE);
	uint64		fresh;

	if (free_word != 0)
	{
		uint32		block = (uint32) (free_word - 1);

		directory.ops->set_region(directory.self, HOST_DELTA_BLOCKS_FREE,
								  directory.ops->block(directory.self, block, HOST_DELTA_BLOCKS_BLOCK_LINKS) &
								  (uint64) PG_UINT32_MAX);
		directory.ops->set_block(directory.self, block, HOST_DELTA_BLOCKS_BLOCK_LINKS, 0);
		*result = block;
		return true;
	}
	fresh = directory.ops->region(directory.self, HOST_DELTA_BLOCKS_FRESH);
	if (!(fresh < (uint64) directory.ops->blocks(directory.self)))
		return false;
	directory.ops->set_region(directory.self, HOST_DELTA_BLOCKS_FRESH,
							  STD_ADD(u64, fresh, 1, 208, 53));
	*result = (uint32) fresh;
	return true;
}

/* host_delta_blocks_pushed puts a block first in its class's list. */
static void
host_delta_blocks_pushed(HostDeltaDirectoryDirectory directory, HostDeltaBlocksRegion list, uint32 block)
{
	uint64		next = directory.ops->region(directory.self, list);

	if (next != 0)
	{
		uint32		after = (uint32) (next - 1);
		uint64		links = directory.ops->block(directory.self, after, HOST_DELTA_BLOCKS_BLOCK_LINKS);

		directory.ops->set_block(directory.self, after, HOST_DELTA_BLOCKS_BLOCK_LINKS,
								 (links & (uint64) PG_UINT32_MAX) |
								 STD_SHL_FOLDED(u64, STD_ADD(u64, (uint64) block, 1, 220, 87), 32));
	}
	directory.ops->set_block(directory.self, block, HOST_DELTA_BLOCKS_BLOCK_LINKS, next);
	directory.ops->set_region(directory.self, list, STD_ADD(u64, (uint64) block, 1, 223, 32));
}

/* host_delta_blocks_unlinked takes a block out of its class's list. */
static void
host_delta_blocks_unlinked(HostDeltaDirectoryDirectory directory, HostDeltaBlocksRegion list, uint32 block)
{
	uint64		links = directory.ops->block(directory.self, block, HOST_DELTA_BLOCKS_BLOCK_LINKS);
	uint64		next = links & (uint64) PG_UINT32_MAX;
	uint64		before = STD_SHR_FOLDED(u64, links, 32);

	if (before != 0)
	{
		uint32		before_block = (uint32) (before - 1);
		uint64		kept = STD_SHL_FOLDED(u64, STD_SHR_FOLDED(u64, directory.ops->block(directory.self, before_block,
																						HOST_DELTA_BLOCKS_BLOCK_LINKS), 32), 32);

		directory.ops->set_block(directory.self, before_block, HOST_DELTA_BLOCKS_BLOCK_LINKS, kept | next);
	}
	else
		directory.ops->set_region(directory.self, list, next);
	if (next != 0)
	{
		uint32		after = (uint32) (next - 1);
		uint64		kept = directory.ops->block(directory.self, after, HOST_DELTA_BLOCKS_BLOCK_LINKS) &
			(uint64) PG_UINT32_MAX;

		directory.ops->set_block(directory.self, after, HOST_DELTA_BLOCKS_BLOCK_LINKS,
								 kept | STD_SHL_FOLDED(u64, before, 32));
	}
	directory.ops->set_block(directory.self, block, HOST_DELTA_BLOCKS_BLOCK_LINKS, 0);
}

#ifdef DOLTGRES_TEST_HOST

/* host_delta_blocks_tests_VecU32_Debug_fmt is a Vec<u32>'s Debug: its elements as a list. */
static bool
host_delta_blocks_tests_VecU32_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = (const StdVec *) value;
	StdDebugList b = std_fmt_debug_list(f);

	for (size_t i = 0; i < v->len; i++)
		std_DebugList_entry(&b, &STD_VEC_AT(v, uint32, i), std_u32_Debug_fmt);
	return std_DebugList_finish(&b);
}

/* host_delta_blocks_tests_Array6U32_Debug_fmt is a [u32; 6]'s Debug: its elements as a list. */
static bool
host_delta_blocks_tests_Array6U32_Debug_fmt(const void *value, StdFormatter *f)
{
	const uint32 *a = (const uint32 *) value;
	StdDebugList b = std_fmt_debug_list(f);

	for (int i = 0; i < 6; i++)
		std_DebugList_entry(&b, &a[i], std_u32_Debug_fmt);
	return std_DebugList_finish(&b);
}

/*
 * test_host_delta_blocks_tests_a_body_takes_the_smallest_class_that_holds_it:
 * a body's class is the power of two from 1 KiB that holds it, a block holds
 * 64 of the smallest and one of the largest, and a full block's bitmap is its
 * every slot (bugs row 150).
 */
void
test_host_delta_blocks_tests_a_body_takes_the_smallest_class_that_holds_it(void)
{
	static const uint64 bodies[] = {1, (uint64) 1 << 10, ((uint64) 1 << 10) + 1, (uint64) 8 << 10,
	((uint64) 8 << 10) + 1, (uint64) 64 << 10};
	static const uint32 expected[] = {0, 0, 1, 3, 4, 6};
	StdSizeHint hint = {lengthof(bodies), true, lengthof(bodies)};
	StdVec		classes;
	volatile bool classes_live;
	bool		equal;
	StdUnwind	unwind;

	std_vec_with_capacity_trusted(&classes, hint, sizeof(uint32), _Alignof(uint32));
	classes_live = true;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		size_t		start = 0;
		size_t		i;

		std_vec_extend_trusted_reserve(&classes, hint, sizeof(uint32), _Alignof(uint32));
		std_array_IntoIter_fold_begin(start, lengthof(bodies));
		while (std_array_IntoIter_fold_next(&start, lengthof(bodies), lengthof(bodies), &i))
		{
			STD_VEC_AT(&classes, uint32, classes.len) = host_delta_blocks_class(bodies[i]);
			classes.len++;
		}
		equal = classes.len == lengthof(expected) && memcmp(classes.ptr, expected, sizeof(expected)) == 0;
		STD_ASSERT_EQ(254, 9, equal, &classes, host_delta_blocks_tests_VecU32_Debug_fmt, expected,
					  host_delta_blocks_tests_Array6U32_Debug_fmt);
		{
			uint64		left_0 = host_delta_blocks_full(0);
			uint64		left_1 = host_delta_blocks_full(3);
			uint64		left_2 = host_delta_blocks_full(6);
			uint64		right_0 = PG_UINT64_MAX;
			uint64		right_1 = 0xff;
			uint64		right_2 = 1;
			StdTupleRef left_ = {3, {STD_ITEM(&left_0, std_u64_Debug_fmt), STD_ITEM(&left_1, std_u64_Debug_fmt),
			STD_ITEM(&left_2, std_u64_Debug_fmt)}};
			StdTupleRef right_ = {3, {STD_ITEM(&right_0, std_u64_Debug_fmt), STD_ITEM(&right_1, std_u64_Debug_fmt),
			STD_ITEM(&right_2, std_u64_Debug_fmt)}};

			STD_ASSERT_EQ(255, 9, left_0 == right_0 && left_1 == right_1 && left_2 == right_2, &left_,
						  std_tuple_Debug_fmt, &right_, std_tuple_Debug_fmt);
		}
		classes_live = false;
		std_vec_free_Checked(&classes, sizeof(uint32), _Alignof(uint32));
		std_unwind_pop(&unwind);
	}
	else
	{
		if (classes_live)
			std_vec_free_Checked(&classes, sizeof(uint32), _Alignof(uint32));
		std_unwind_resume(&unwind);
	}
}

#endif							/* DOLTGRES_TEST_HOST */
