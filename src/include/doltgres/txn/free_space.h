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
 * A storage's free space map (txn's free_space.rs): each block's free bytes
 * and freed offsets, kept in the base's free space tree, and their
 * categories as heap's free space map rounds them.
 */
#ifndef DOLTGRES_TXN_FREE_SPACE_H
#define DOLTGRES_TXN_FREE_SPACE_H

#include "doltgres/chunks/lib.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/trees/lib.h"

/* TXN_FREE_SPACE_CATEGORIES is the count of free space categories, heap's. */
#define TXN_FREE_SPACE_CATEGORIES 256

/*
 * TXN_FREE_SPACE_MAX_REQUEST is the most free space a request can ask for,
 * heap's MaxFSMRequestSize, its MaxHeapTupleSize: a page less its header and
 * one line pointer, aligned.
 */
#define TXN_FREE_SPACE_MAX_REQUEST ((uint64) CHUNKS_BLCKSZ - 32)

/* TXN_FREE_SPACE_BITMAP_MOST is the most bytes a block's bitmap of freed offsets takes. */
#define TXN_FREE_SPACE_BITMAP_MOST 147

/*
 * TxnFreeSpaceFree is Free: a block's entry in a free space map, its free
 * bytes, the bitmap of its freed offsets, and whether an update found no
 * room on it.
 */
typedef struct TxnFreeSpaceFree
{
	uint64		bytes;
	uint8		offsets[TXN_FREE_SPACE_BITMAP_MOST];
	/* len is the bitmap's length, ceil(most / 8). */
	size_t		len;

	/*
	 * full is whether an update's new row found no room on the block, as
	 * heap's PD_PAGE_FULL is set, until a prune of the block clears it.
	 */
	bool		full;
} TxnFreeSpaceFree;

extern TxnFreeSpaceFree txn_free_space_Free_new(uint64 bytes, const uint16 *free, size_t free_len,
												uint16 most);
extern void txn_free_space_Free_encode(const TxnFreeSpaceFree *self, StdVec *result);
extern void txn_free_space_key(uint32 block, uint8 result[4]);
extern uint64 txn_free_space_avail(uint64 bytes);
extern uint8 txn_free_space_category(uint64 bytes);
extern bool txn_free_space_value_bytes(const uint8 *value, size_t value_len, uint64 *result, HostError *error);
extern TreesShape txn_free_space_shape(void);
extern size_t txn_free_space_most_len(uint16 most);
extern bool txn_free_space_Free_decode(const uint8 *value, size_t value_len, uint32 block, uint64 fresh_end,
									   uint16 most, TxnFreeSpaceFree *result, HostError *error);
extern bool txn_free_space_Free_parsed(const uint8 *value, size_t value_len, uint32 block, uint64 fresh_end,
									   uint16 most, TxnFreeSpaceFree *result);
extern void txn_free_space_Free_encode_into(const TxnFreeSpaceFree *self, StdVec *out);
extern bool txn_free_space_Free_is_clear(const TxnFreeSpaceFree *self);
extern bool txn_free_space_Free_lowest(const TxnFreeSpaceFree *self, uint16 *result);
extern bool txn_free_space_Free_has(const TxnFreeSpaceFree *self, uint16 offset);
extern void txn_free_space_Free_take(TxnFreeSpaceFree *self, uint16 offset, uint64 bytes);
extern void txn_free_space_Free_give_back(TxnFreeSpaceFree *self, uint16 first, uint16 last, uint64 bytes);
extern void txn_free_space_Free_free_from(TxnFreeSpaceFree *self, uint16 first, uint16 most);
extern bool txn_free_space_Free_PartialEq_eq(const TxnFreeSpaceFree *self, const TxnFreeSpaceFree *other);
extern bool txn_free_space_Free_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_free_space_needed(uint64 bytes, uint8 *result, HostError *error);
extern uint64 txn_free_space_threshold(uint8 category);
extern bool txn_free_space_frees(const uint8 *value, size_t value_len, uint16 offset, bool *result,
								 HostError *error);
extern bool txn_free_space_largest(uint8 level, const StdBytes *items, size_t items_len, StdVec *result,
								   ChunksErrorError *error);
extern bool txn_free_space_seek(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, uint32 from,
								uint64 bytes, bool *some, uint32 *block, StdVec *value, HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_free_space_tests_a_free_space_entry_reads_back_and_refuses_every_other_form(void);
extern void test_txn_free_space_tests_an_insert_takes_the_lowest_free_offset_and_a_left_fresh_page_frees_its_counter_offsets(void);
extern void test_txn_free_space_tests_a_seek_finds_the_lowest_block_with_room_through_the_summaries(void);
extern void test_txn_free_space_tests_an_entry_past_the_bitmap_panics_at_encode(void);
extern void test_txn_free_space_tests_an_entry_past_the_bitmap_panics_at_is_clear(void);
extern void test_txn_free_space_tests_an_entry_past_the_bitmap_panics_at_lowest(void);
#endif

#endif							/* DOLTGRES_TXN_FREE_SPACE_H */
