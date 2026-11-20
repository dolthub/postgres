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
 * The C port of chunks' packs/follow.rs: the forwards of places a retired
 * pack's chunks moved from, kept under keys that sort by pack and then by
 * offset.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/follow.h"
#include "doltgres/std/num.h"

/*
 * chunks_packs_follow_place_key returns the key a place's forward is kept
 * under: the pack's number, then the offset, each with its highest byte
 * first, and zeros after them.
 */
ChunksAddressAddress
chunks_packs_follow_place_key(const std_TupleU64U64_t *from)
{
	ChunksAddressAddress key = {0};

	key.f0[0] = (uint8) STD_SHR_FOLDED(u64, from->f0, 56);
	key.f0[1] = (uint8) STD_SHR_FOLDED(u64, from->f0, 48);
	key.f0[2] = (uint8) STD_SHR_FOLDED(u64, from->f0, 40);
	key.f0[3] = (uint8) STD_SHR_FOLDED(u64, from->f0, 32);
	key.f0[4] = (uint8) STD_SHR_FOLDED(u64, from->f0, 24);
	key.f0[5] = (uint8) STD_SHR_FOLDED(u64, from->f0, 16);
	key.f0[6] = (uint8) STD_SHR_FOLDED(u64, from->f0, 8);
	key.f0[7] = (uint8) from->f0;
	key.f0[8] = (uint8) STD_SHR_FOLDED(u64, from->f1, 56);
	key.f0[9] = (uint8) STD_SHR_FOLDED(u64, from->f1, 48);
	key.f0[10] = (uint8) STD_SHR_FOLDED(u64, from->f1, 40);
	key.f0[11] = (uint8) STD_SHR_FOLDED(u64, from->f1, 32);
	key.f0[12] = (uint8) STD_SHR_FOLDED(u64, from->f1, 24);
	key.f0[13] = (uint8) STD_SHR_FOLDED(u64, from->f1, 16);
	key.f0[14] = (uint8) STD_SHR_FOLDED(u64, from->f1, 8);
	key.f0[15] = (uint8) from->f1;
	return key;
}

/*
 * chunks_packs_follow_Indexed_ask is Indexed's Forwards::ask: the forward
 * of a place, the one the newest run with its key holds, read through the
 * source's blocks, with where it was read.
 */
static bool
chunks_packs_follow_Indexed_ask(void *self, void *with, const std_TupleU64U64_t *from, bool *has_found,
								std_TupleUsizeUsize_t *read, ChunksTrailerLocator *to, void *error)
{
	ChunksPacksFollowIndexed *indexed = self;
	ChunksAddressAddress key = chunks_packs_follow_place_key(from);

	return chunks_index_first_in_index_LocatorError(indexed->source, with, indexed->fences.ptr, indexed->fences.len,
													&key, has_found, read, to, error);
}

/* CHUNKS_PACKS_FOLLOW_INDEXED_FORWARDS_OPS is Indexed's Forwards, whose error is an Unread<Error>. */
const ChunksPacksFollowForwardsOps CHUNKS_PACKS_FOLLOW_INDEXED_FORWARDS_OPS = {chunks_packs_follow_Indexed_ask};

/*
 * chunks_packs_follow_follow returns the last place of a copy that later
 * collections moved again, through the forwards, stopping at a pack the
 * table lists. Each forward must lead to a pack numbered higher and no
 * higher than the highest added; one that doesn't stops the walk where it
 * was read, and a failed ask stops it with the ask's error in error.
 */
bool
chunks_packs_follow_follow(ChunksPacksFollowForwards forwards, void *with, ChunksTrailerLocator copy,
						   const StdVec *listed, uint64 added, ChunksTrailerLocator *result,
						   ChunksPacksFollowStopped *stopped, void *error)
{
	ChunksTrailerLocator at = copy;

	for (;;)
	{
		std_TupleU64U64_t from;
		bool		has_found;
		std_TupleUsizeUsize_t read;
		ChunksTrailerLocator next;

		if (chunks_packs_table_packs_get(listed, at.pack) != NULL)
		{
			*result = at;
			return true;
		}
		from.f0 = at.pack;
		from.f1 = chunks_trailer_Locator_offset(&at);
		if (!forwards.ops->ask(forwards.self, with, &from, &has_found, &read, &next, error))
		{
			stopped->tag = CHUNKS_PACKS_FOLLOW_STOPPED_SOURCE;
			return false;
		}
		if (!has_found)
		{
			*result = at;
			return true;
		}
		if (next.pack <= at.pack || next.pack > added)
		{
			stopped->tag = CHUNKS_PACKS_FOLLOW_STOPPED_FALLS;
			stopped->read = read;
			stopped->from = at;
			stopped->to = next;
			return false;
		}
		at = next;
	}
}
