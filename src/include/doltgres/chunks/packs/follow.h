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
 * The forwards of places a retired pack's chunks moved from (chunks'
 * packs/follow.rs): the keys they are kept under.
 */
#ifndef DOLTGRES_CHUNKS_PACKS_FOLLOW_H
#define DOLTGRES_CHUNKS_PACKS_FOLLOW_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/index.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/std/tuple.h"

/* ChunksPacksFollowForwardsOps is the Forwards trait: the forward of a place, if one leads on, with where it was read. */
typedef struct ChunksPacksFollowForwardsOps
{
	bool		(*ask) (void *self, void *with, const std_TupleU64U64_t *from, bool *has_found,
						std_TupleUsizeUsize_t *read, ChunksTrailerLocator *to, void *error);
} ChunksPacksFollowForwardsOps;

/* ChunksPacksFollowForwards is a value that implements Forwards. */
typedef struct ChunksPacksFollowForwards
{
	const ChunksPacksFollowForwardsOps *ops;
	void	   *self;
} ChunksPacksFollowForwards;

/*
 * ChunksPacksFollowIndexed is Indexed: the forwards an index's runs hold,
 * each under its place's key, a source of the runs' blocks and each run's
 * fences, a Vec<&Fences>, oldest first.
 */
typedef struct ChunksPacksFollowIndexed
{
	ChunksIndexSource source;
	StdVec		fences;
} ChunksPacksFollowIndexed;

/* ChunksPacksFollowStoppedTag is Stopped's variants: why a walk of forwards ended without its last place. */
typedef enum ChunksPacksFollowStoppedTag
{
	CHUNKS_PACKS_FOLLOW_STOPPED_SOURCE,
	CHUNKS_PACKS_FOLLOW_STOPPED_FALLS,
} ChunksPacksFollowStoppedTag;

/*
 * ChunksPacksFollowStopped is Stopped: a failed read, whose error the walk
 * writes to its caller's place, or a forward that falls, read at a run's and
 * a block's positions, from a place to a copy.
 */
typedef struct ChunksPacksFollowStopped
{
	ChunksPacksFollowStoppedTag tag;
	std_TupleUsizeUsize_t read;
	ChunksTrailerLocator from;
	ChunksTrailerLocator to;
} ChunksPacksFollowStopped;

extern const ChunksPacksFollowForwardsOps CHUNKS_PACKS_FOLLOW_INDEXED_FORWARDS_OPS;

extern ChunksAddressAddress chunks_packs_follow_place_key(const std_TupleU64U64_t *from);

/*
 * chunks_packs_follow_follow is follow: its listed map is the pack table's,
 * a key-ordered StdVec of chunks_TupleU64Listed_t, follow's one instance.
 */
extern bool chunks_packs_follow_follow(ChunksPacksFollowForwards forwards, void *with, ChunksTrailerLocator copy,
									   const StdVec *listed, uint64 added, ChunksTrailerLocator *result,
									   ChunksPacksFollowStopped *stopped, void *error);

#endif							/* DOLTGRES_CHUNKS_PACKS_FOLLOW_H */
