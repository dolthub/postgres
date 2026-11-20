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
 * An index's deletion of the entries a table's rows no longer need
 * (doltgres' deletes.rs), heap's heap_index_delete_tuples over the fork's
 * tuple ids, in steps its caller drives.
 */
#ifndef DOLTGRES_DOLTGRES_DELETES_H
#define DOLTGRES_DOLTGRES_DELETES_H

#include "doltgres/std/vec.h"

/*
 * DoltgresDeletesDeltid is Deltid: an entry of the caller's array, its row's
 * tuple id, its place in the caller's status array, whether it is known
 * deletable, whether the caller marked it promising, and the index space its
 * deletion frees.
 */
typedef struct DoltgresDeletesDeltid
{
	uint64		tid;
	int16		id;
	bool		known;
	bool		promising;
	int16		freespace;
} DoltgresDeletesDeltid;

/*
 * DoltgresDeletesLooked is Looked: what the table says of an entry's row,
 * whether a NonVacuumable snapshot still sees it, and, for a row some layer
 * holds, its writer, whether that committed, and the xid of the update or
 * delete that ended it.
 */
typedef struct DoltgresDeletesLooked
{
	bool		kept;
	bool		has_row;
	struct
	{
		uint32		f0;
		bool		f1;
		bool		has_f2;
		uint32		f2;
	}			row;
} DoltgresDeletesLooked;

/*
 * DoltgresDeletesDeleting is Deleting: a deletion in progress, its entries,
 * the next entry, the count of entries up to the last one answered
 * deletable, the conflict horizon so far, and a bottom-up caller's space
 * target, freed space, and block counts.
 */
typedef struct DoltgresDeletesDeleting
{
	/* entries is a Vec<Deltid>. */
	StdVec		entries;
	size_t		at;
	size_t		kept;
	uint32		horizon;
	bool		has_block;
	uint64		block;
	size_t		accessed;
	bool		has_target;
	int32		target;
	int32		wanted;
	int32		freed;
	int32		last;
	bool		last_block;
	size_t		favorable;
} DoltgresDeletesDeleting;

extern void doltgres_deletes_Deleting_new(StdVec *entries, bool has_target, int32 target,
										  DoltgresDeletesDeleting *result);
extern bool doltgres_deletes_Deleting_next(DoltgresDeletesDeleting *self, uint64 *result);
extern void doltgres_deletes_Deleting_answer(DoltgresDeletesDeleting *self, DoltgresDeletesLooked looked);
extern void doltgres_deletes_Deleting_finish(DoltgresDeletesDeleting *self, StdVec *result_0, uint32 *result_1);

#ifdef DOLTGRES_TEST_DOLTGRES
extern void test_doltgres_deletes_tests_a_simple_deletion_marks_the_rows_no_snapshot_keeps(void);
extern void test_doltgres_deletes_tests_a_bottom_up_deletion_visits_heaps_blocks_in_heaps_order(void);
#endif

#endif							/* DOLTGRES_DOLTGRES_DELETES_H */
