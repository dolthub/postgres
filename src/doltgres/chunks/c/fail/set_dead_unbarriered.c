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
 * set_dead_unbarriered: a pack's deletion vector set without the barrier
 * held exclusively is refused by the capability analysis, since only under
 * it does no marking's pass meet a vector written and not yet named (bugs
 * row 499, H14).
 */
#include "postgres.h"

#include "doltgres/chunks/packs/retire.h"

/* set_dead_unbarriered_case sets a vector holding nothing. */
extern bool set_dead_unbarriered_case(ChunksPacksRetireMarking *marking, const StdBTreeMap *offsets, bool *set,
									  ChunksErrorError *error);

bool
set_dead_unbarriered_case(ChunksPacksRetireMarking *marking, const StdBTreeMap *offsets, bool *set,
						  ChunksErrorError *error)
{
	return chunks_packs_retire_Marking_set_dead(marking, 1, 2, offsets, set, error);
}
