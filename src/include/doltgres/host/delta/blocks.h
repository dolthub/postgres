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
 * The delta's region of blocks (host's delta/blocks.rs, bugs row 116): its
 * words, a block's words, and the take and give of a page's body, which the
 * page directory calls.
 */
#ifndef DOLTGRES_HOST_DELTA_BLOCKS_H
#define DOLTGRES_HOST_DELTA_BLOCKS_H

#include "doltgres/host/delta/page.h"

/*
 * HOST_DELTA_BLOCKS_CLASSES is the count of page sizes a block holds: 1, 2,
 * 4, 8, 16, 32, and 64 KiB (bugs row 150).
 */
#define HOST_DELTA_BLOCKS_CLASSES 7

/* HostDeltaBlocksRegionTag is Region's variants: one of the region's words. */
typedef enum HostDeltaBlocksRegionTag
{
	/*
	 * HOST_DELTA_BLOCKS_REGION_FREE is one more than the block on top of the
	 * free stack, or zero while it is empty.
	 */
	HOST_DELTA_BLOCKS_REGION_FREE,

	/*
	 * HOST_DELTA_BLOCKS_REGION_FRESH is the count of blocks taken from the
	 * region's end.
	 */
	HOST_DELTA_BLOCKS_REGION_FRESH,
	/* HOST_DELTA_BLOCKS_REGION_TAKEN is the count of blocks holding a page. */
	HOST_DELTA_BLOCKS_REGION_TAKEN,

	/*
	 * HOST_DELTA_BLOCKS_REGION_OVER is one more than the block a take past
	 * the budget took, or zero for none.
	 */
	HOST_DELTA_BLOCKS_REGION_OVER,

	/*
	 * HOST_DELTA_BLOCKS_REGION_PARTIAL is one more than the first block of a
	 * class with a free slot and a page.
	 */
	HOST_DELTA_BLOCKS_REGION_PARTIAL,
}			HostDeltaBlocksRegionTag;

/* HostDeltaBlocksRegion is one of the region's words, which change only under the region's lock. */
typedef struct HostDeltaBlocksRegion
{
	HostDeltaBlocksRegionTag tag;
	union
	{
		uint32		partial;
	}			u;
}			HostDeltaBlocksRegion;

/* HOST_DELTA_BLOCKS_WORDS is the count of the region's words. */
#define HOST_DELTA_BLOCKS_WORDS (4 + HOST_DELTA_BLOCKS_CLASSES)

/* HostDeltaBlocksBlock is one of a block's three words. */
typedef enum HostDeltaBlocksBlock
{
	/* HOST_DELTA_BLOCKS_BLOCK_SLOTS is the block's class. */
	HOST_DELTA_BLOCKS_BLOCK_SLOTS,

	/*
	 * HOST_DELTA_BLOCKS_BLOCK_MAP is the bitmap of the block's taken slots, a
	 * bit for each of its smallest class's 64 (bugs row 150).
	 */
	HOST_DELTA_BLOCKS_BLOCK_MAP,

	/*
	 * HOST_DELTA_BLOCKS_BLOCK_LINKS is one more than the next block of its
	 * list or stack in its low half, and one more than the one before it in
	 * its class's list in its high half, or zero for none.
	 */
	HOST_DELTA_BLOCKS_BLOCK_LINKS,
}			HostDeltaBlocksBlock;

/* The page directory's Directory, which the functions take; directory.h includes this file for the types above. */
struct HostDeltaDirectoryDirectory;

extern size_t host_delta_blocks_Region_index(HostDeltaBlocksRegion self);
extern uint32 host_delta_blocks_class(uint64 body);
extern uint64 host_delta_blocks_used(struct HostDeltaDirectoryDirectory directory);
extern bool host_delta_blocks_take(struct HostDeltaDirectoryDirectory directory, HostDeltaPageSize size,
								   bool overshooting, uint64 *result);
extern void host_delta_blocks_give(struct HostDeltaDirectoryDirectory directory, uint64 body);

#ifdef DOLTGRES_TEST_HOST
extern void test_host_delta_blocks_tests_a_body_takes_the_smallest_class_that_holds_it(void);
#endif

#endif							/* DOLTGRES_HOST_DELTA_BLOCKS_H */
