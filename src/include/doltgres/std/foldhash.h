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
 * The C form of foldhash 0.2.0's fast hasher (the port text's section 15):
 * FoldHasher, and fast::RandomState, seeded per process as foldhash seeds
 * it, from the global seed and a per-hasher seed.
 */
#ifndef DOLTGRES_STD_FOLDHASH_H
#define DOLTGRES_STD_FOLDHASH_H

#include "doltgres/std/hash.h"

/* FoldhashSharedSeed is foldhash's SharedSeed: six seeds every hasher of the process folds with. */
typedef struct FoldhashSharedSeed
{
	uint64		seeds[6];
} FoldhashSharedSeed;

/*
 * FoldhashFastFoldHasher is fast::FoldHasher: its accumulator, the sponge
 * of small integers not yet folded and its bits, and the shared seeds.
 */
typedef struct FoldhashFastFoldHasher
{
	uint64		accumulator;
	StdU128		sponge;
	uint8		sponge_len;
	const uint64 *seeds;
} FoldhashFastFoldHasher;

/* FoldhashFastRandomState is fast::RandomState: its per-hasher seed, the global seed being the process's. */
typedef struct FoldhashFastRandomState
{
	uint64		per_hasher_seed;
} FoldhashFastRandomState;

extern const StdHasherOps FOLDHASH_FAST_FOLD_HASHER_HASHER_OPS;
extern const StdBuildHasherOps FOLDHASH_FAST_RANDOM_STATE_BUILD_HASHER_OPS;

extern void foldhash_fast_RandomState_Default_default(void *result);

#endif							/* DOLTGRES_STD_FOLDHASH_H */
