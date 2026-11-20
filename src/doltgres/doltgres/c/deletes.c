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
 * tuple ids: the entries sorted by tuple id, a bottom-up caller's blocks
 * ordered and cut as bottomup_sort_and_shrink orders them, each entry's row
 * asked whether a NonVacuumable snapshot still sees it, and the conflict
 * horizon advanced over each deletable entry's row, in steps its caller
 * drives.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/doltgres/deletes.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/unwind.h"
#ifdef DOLTGRES_TEST_DOLTGRES
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "deletes.rs"

/* DOLTGRES_DELETES_BOTTOMUP_MAX_NBLOCKS is heap's most blocks a bottom-up deletion visits. */
#define DOLTGRES_DELETES_BOTTOMUP_MAX_NBLOCKS 6

/* DOLTGRES_DELETES_BOTTOMUP_TOLERANCE_NBLOCKS is how far out of order heap still counts a block as favorable. */
#define DOLTGRES_DELETES_BOTTOMUP_TOLERANCE_NBLOCKS 3

/* The element layouts of the deletion's vectors. */
#define DOLTGRES_DELETES_DELTID sizeof(DoltgresDeletesDeltid), _Alignof(DoltgresDeletesDeltid)
#define DOLTGRES_DELETES_GROUP sizeof(doltgres_TupleUsizeU32U32_t), _Alignof(doltgres_TupleUsizeU32U32_t)

/*
 * doltgres_TupleUsizeU32U32_t is sort_and_shrink's (usize, u32, u32): a
 * block's first entry, its entries, and its promising ones.
 */
typedef struct doltgres_TupleUsizeU32U32_t
{
	size_t		f0;
	uint32		f1;
	uint32		f2;
} doltgres_TupleUsizeU32U32_t;

/*
 * DoltgresDeletesGroupKey is sort_and_shrink's key (Reverse(u32),
 * Reverse(u32), usize): a block's promising bucket and its entries' power of
 * two, both descending, then its first entry.
 */
typedef struct DoltgresDeletesGroupKey
{
	uint32		promising;
	uint32		count;
	size_t		first;
} DoltgresDeletesGroupKey;

/*
 * doltgres_deletes_GroupKey_lt is the key's PartialOrd::lt, a tuple's: its
 * Reverses' descending order, then its first's.
 */
static bool
doltgres_deletes_GroupKey_lt(const DoltgresDeletesGroupKey *a, const DoltgresDeletesGroupKey *b)
{
	if (a->promising != b->promising)
		return a->promising > b->promising;
	if (a->count != b->count)
		return a->count > b->count;
	return a->first < b->first;
}

/* The stable sort_by_key of the entries by tuple id: core's shared sort parts at Deltid, then its stable sort. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS Deltid
#define STD_SORT_T DoltgresDeletesDeltid
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"
#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS Deltid
#define STD_STABLE_SORT_T DoltgresDeletesDeltid
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_WITHOUT_SORT
#define STD_STABLE_SORT_WITH_BY_KEY
#define STD_STABLE_SORT_KEY_KS U64
#define STD_STABLE_SORT_KEY_K uint64
#define STD_STABLE_SORT_KEY_LT(a, b) (*(a) < *(b))
#include "doltgres/std/stable_sort_template.h"

/*
 * The stable sort_by_key of a bottom-up deletion's blocks by their key: core's
 * shared sort parts, then its stable sort.
 */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS TupleUsizeU32U32
#define STD_SORT_T doltgres_TupleUsizeU32U32_t
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"
#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS TupleUsizeU32U32
#define STD_STABLE_SORT_T doltgres_TupleUsizeU32U32_t
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_WITHOUT_SORT
#define STD_STABLE_SORT_WITH_BY_KEY
#define STD_STABLE_SORT_KEY_KS GroupKey
#define STD_STABLE_SORT_KEY_K DoltgresDeletesGroupKey
#define STD_STABLE_SORT_KEY_LT(a, b) doltgres_deletes_GroupKey_lt((a), (b))
#include "doltgres/std/stable_sort_template.h"

/*
 * doltgres_deletes_Deleting_new_closure_0 is new's |entry| entry.tid: an
 * entry's tuple id, the key its sort orders by.
 */
static uint64
doltgres_deletes_Deleting_new_closure_0(void *captures, const void *entry)
{
	return ((const DoltgresDeletesDeltid *) entry)->tid;
}

/*
 * doltgres_deletes_sort_and_shrink_closure_0 is sort_and_shrink's key: a
 * block's promising bucket and its count's power of two, descending, then its
 * first entry.
 */
static DoltgresDeletesGroupKey
doltgres_deletes_sort_and_shrink_closure_0(void *captures, const void *group_)
{
	const doltgres_TupleUsizeU32U32_t *group = group_;
	DoltgresDeletesGroupKey key = {group->f2, std_next_power_of_two_u32(group->f1, STD_NUM_MOD_FILE, 1290, 5),
	group->f0};

	return key;
}

/*
 * doltgres_deletes_follows reports whether an xid follows another, Postgres'
 * TransactionIdFollows: modulo 2^32 for two normal xids, by value otherwise.
 */
static bool
doltgres_deletes_follows(uint32 a, uint32 b)
{
	if (a < 3 || b < 3)
		return a > b;
	return (int32) (a - b) > 0;
}

/*
 * doltgres_deletes_sort_and_shrink_frame_t is sort_and_shrink's state across
 * its pad: the blocks and the reordered entries, each while live.
 */
typedef struct doltgres_deletes_sort_and_shrink_frame_t
{
	StdVec	   *entries;
	StdVec		groups;
	bool		groups_live;
	StdVec		reordered;
	bool		reordered_live;
	size_t		favorable;
} doltgres_deletes_sort_and_shrink_frame_t;

/*
 * doltgres_deletes_sort_and_shrink_body groups the entries by block, orders
 * and cuts the blocks, and reorders the entries.
 */
static void
doltgres_deletes_sort_and_shrink_body(void *frame_)
{
	doltgres_deletes_sort_and_shrink_frame_t *frame = frame_;
	StdVec	   *entries = frame->entries;
	bool		has_previous = false;
	int64		previous = 0;
	size_t		total = 0;

	std_vec_new(&frame->groups, DOLTGRES_DELETES_GROUP);
	frame->groups_live = true;
	for (size_t at = 0; at < entries->len; at++)
	{
		const DoltgresDeletesDeltid *entry = &STD_VEC_AT(entries, DoltgresDeletesDeltid, at);
		doltgres_TupleUsizeU32U32_t *last = frame->groups.len > 0 ?
			&STD_VEC_AT(&frame->groups, doltgres_TupleUsizeU32U32_t, frame->groups.len - 1) : NULL;

		if (last != NULL && STD_VEC_AT(entries, DoltgresDeletesDeltid, last->f0).tid >> 16 == entry->tid >> 16)
		{
			last->f1 = STD_ADD(u32, last->f1, 1, 163, 17);
			last->f2 = STD_ADD(u32, last->f2, entry->promising ? 1 : 0, 164, 17);
		}
		else
		{
			doltgres_TupleUsizeU32U32_t group = {at, 1, entry->promising ? 1 : 0};

			std_vec_push_Checked(&frame->groups, &group, DOLTGRES_DELETES_GROUP);
		}
	}
	for (size_t i = 0; i < frame->groups.len; i++)
	{
		doltgres_TupleUsizeU32U32_t *group = &STD_VEC_AT(&frame->groups, doltgres_TupleUsizeU32U32_t, i);

		group->f2 = group->f2 <= 4 ? 4 : std_next_power_of_two_u32(group->f2, STD_NUM_MOD_FILE, 1290, 5);
	}
	std_slice_sort_by_key_TupleUsizeU32U32GroupKey(frame->groups.ptr, frame->groups.len,
												   doltgres_deletes_sort_and_shrink_closure_0, NULL, NULL);
	frame->groups.len = Min(frame->groups.len, DOLTGRES_DELETES_BOTTOMUP_MAX_NBLOCKS);
	for (size_t i = 0; i < frame->groups.len; i++)
	{
		size_t		first = STD_VEC_AT(&frame->groups, doltgres_TupleUsizeU32U32_t, i).f0;
		int64		block = (int64) (STD_VEC_AT(entries, DoltgresDeletesDeltid, first).tid >> 16);

		if (has_previous && (block < previous - DOLTGRES_DELETES_BOTTOMUP_TOLERANCE_NBLOCKS ||
							 block > previous + DOLTGRES_DELETES_BOTTOMUP_TOLERANCE_NBLOCKS))
			break;
		frame->favorable++;
		has_previous = true;
		previous = block;
	}
	for (size_t i = 0; i < frame->groups.len; i++)
		total += STD_VEC_AT(&frame->groups, doltgres_TupleUsizeU32U32_t, i).f1;
	std_vec_with_capacity_Checked(&frame->reordered, total, DOLTGRES_DELETES_DELTID);
	frame->reordered_live = true;
	for (size_t i = 0; i < frame->groups.len; i++)
	{
		const doltgres_TupleUsizeU32U32_t *group = &STD_VEC_AT(&frame->groups, doltgres_TupleUsizeU32U32_t, i);

		std_vec_extend_from_slice_Checked(&frame->reordered, &STD_VEC_AT(entries, DoltgresDeletesDeltid, group->f0),
										  group->f1, DOLTGRES_DELETES_DELTID);
	}
	std_vec_free_Checked(entries, DOLTGRES_DELETES_DELTID);
	*entries = frame->reordered;
	frame->reordered_live = false;
}

/* doltgres_deletes_sort_and_shrink_pad frees what sort_and_shrink holds live: the reordered entries and the blocks. */
static void
doltgres_deletes_sort_and_shrink_pad(void *frame_)
{
	doltgres_deletes_sort_and_shrink_frame_t *frame = frame_;

	if (frame->reordered_live)
		std_vec_free_Checked(&frame->reordered, DOLTGRES_DELETES_DELTID);
	if (frame->groups_live)
		std_vec_free_Checked(&frame->groups, DOLTGRES_DELETES_GROUP);
	frame->reordered_live = frame->groups_live = false;
}

/*
 * doltgres_deletes_sort_and_shrink orders a bottom-up caller's entries by
 * block, as heap's bottomup_sort_and_shrink does: the blocks with the most
 * promising entries first, in power-of-two buckets, then the most entries,
 * then the lowest block, cut to BOTTOMUP_MAX_NBLOCKS blocks; it returns how
 * many blocks from the start are favorable.
 */
static size_t
doltgres_deletes_sort_and_shrink(StdVec *entries)
{
	doltgres_deletes_sort_and_shrink_frame_t frame = {.entries = entries};

	std_landing(&frame, doltgres_deletes_sort_and_shrink_body, doltgres_deletes_sort_and_shrink_pad);
	doltgres_deletes_sort_and_shrink_pad(&frame);
	return frame.favorable;
}

/*
 * doltgres_deletes_Deleting_new_frame_t is new's state across its pad: the
 * deletion it makes, whose entries it takes.
 */
typedef struct doltgres_deletes_Deleting_new_frame_t
{
	DoltgresDeletesDeleting deleting;
	bool		entries_live;
} doltgres_deletes_Deleting_new_frame_t;

/* doltgres_deletes_Deleting_new_body sorts the entries by tuple id, then orders a bottom-up caller's by block. */
static void
doltgres_deletes_Deleting_new_body(void *frame_)
{
	doltgres_deletes_Deleting_new_frame_t *frame = frame_;
	DoltgresDeletesDeleting *deleting = &frame->deleting;

	std_slice_sort_by_key_DeltidU64(deleting->entries.ptr, deleting->entries.len,
									doltgres_deletes_Deleting_new_closure_0, NULL, NULL);
	if (deleting->has_target)
		deleting->favorable = doltgres_deletes_sort_and_shrink(&deleting->entries);
	frame->entries_live = false;
}

/* doltgres_deletes_Deleting_new_pad frees new's entries, while live. */
static void
doltgres_deletes_Deleting_new_pad(void *frame_)
{
	doltgres_deletes_Deleting_new_frame_t *frame = frame_;

	if (frame->entries_live)
		std_vec_free_Checked(&frame->deleting.entries, DOLTGRES_DELETES_DELTID);
	frame->entries_live = false;
}

/*
 * doltgres_deletes_Deleting_new sorts a caller's entries by tuple id, taking
 * them, and orders a bottom-up caller's by block; target is a bottom-up
 * caller's space target, none for a simple deletion.
 */
void
doltgres_deletes_Deleting_new(StdVec *entries, bool has_target, int32 target, DoltgresDeletesDeleting *result)
{
	doltgres_deletes_Deleting_new_frame_t frame = {.deleting = {.entries = *entries, .has_target = has_target,
	.target = target, .wanted = has_target ? target : 0}, .entries_live = true};

	std_landing(&frame, doltgres_deletes_Deleting_new_body, doltgres_deletes_Deleting_new_pad);
	doltgres_deletes_Deleting_new_pad(&frame);
	*result = frame.deleting;
}

/*
 * doltgres_deletes_Deleting_next returns the tuple id of the next entry
 * whose row the deletion asks about, or false once it stops: at the end, or,
 * for a bottom-up caller, at a new block once the target is met or the last
 * block freed nothing.
 */
bool
doltgres_deletes_Deleting_next(DoltgresDeletesDeleting *self, uint64 *result)
{
	DoltgresDeletesDeltid entry;

	if (self->at >= self->entries.len)
		return false;
	entry = STD_VEC_AT(&self->entries, DoltgresDeletesDeltid, self->at);
	if (!self->has_block || self->block != entry.tid >> 16)
	{
		if (self->has_target)
		{
			if (self->last_block || (self->accessed >= 1 && self->freed == self->last))
				return false;
			self->last = self->freed;
			if (self->favorable > 0)
				self->favorable--;
			else
				self->wanted /= 2;
		}
		self->has_block = true;
		self->block = entry.tid >> 16;
		self->accessed++;
	}
	*result = entry.tid;
	return true;
}

/*
 * doltgres_deletes_Deleting_answer takes what the table says of the row of
 * the entry next returned: a row a NonVacuumable snapshot keeps leaves its
 * entry as it was; any other marks it deletable, counts its freed space, and
 * advances the horizon by its committed writer's ender.
 */
void
doltgres_deletes_Deleting_answer(DoltgresDeletesDeleting *self, DoltgresDeletesLooked looked)
{
	size_t		at = self->at;
	DoltgresDeletesDeltid *entry;

	self->at++;
	std_index_check(at, self->entries.len, RUST_FILE, 117, 26);
	entry = &STD_VEC_AT(&self->entries, DoltgresDeletesDeltid, at);
	if (!entry->known)
	{
		if (looked.kept)
			return;
		entry->known = true;
		if (self->has_target)
		{
			self->freed = STD_ADD(i32, self->freed, (int32) entry->freespace, 124, 17);
			self->last_block |= self->freed >= self->wanted;
		}
	}
	if (looked.has_row && looked.row.f1 && looked.row.has_f2 && looked.row.f2 != looked.row.f0 &&
		doltgres_deletes_follows(looked.row.f2, self->horizon))
		self->horizon = looked.row.f2;
	self->kept = at + 1;
}

/*
 * doltgres_deletes_Deleting_finish returns the entries cut after the last
 * one answered deletable, taking the deletion, and the conflict horizon,
 * heap's snapshotConflictHorizon.
 */
void
doltgres_deletes_Deleting_finish(DoltgresDeletesDeleting *self, StdVec *result_0, uint32 *result_1)
{
	self->entries.len = Min(self->entries.len, self->kept);
	*result_0 = self->entries;
	*result_1 = self->horizon;
}

#ifdef DOLTGRES_TEST_DOLTGRES

/*
 * doltgres_deletes_tests_entry returns an entry of a block and an offset, at
 * its place, neither known nor promising, freeing 10 bytes.
 */
static DoltgresDeletesDeltid
doltgres_deletes_tests_entry(uint64 block, uint64 offset, int16 id)
{
	DoltgresDeletesDeltid entry = {.tid = block << 16 | offset, .id = id, .freespace = 10};

	return entry;
}

/* doltgres_deletes_tests_marked_Debug_fmt is a Vec<(u64, bool)>'s Debug, of an array of tuple ids and their marks. */
static bool
doltgres_deletes_tests_marked_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t i = 0; i < v->len; i++)
	{
		const DoltgresDeletesDeltid *entry = &STD_VEC_AT(v, DoltgresDeletesDeltid, i);
		StdTupleRef tuple = {2, {{&entry->tid, std_u64_Debug_fmt}, {&entry->known, std_bool_Debug_fmt}}};

		std_DebugList_entry(&list, &tuple, std_tuple_Debug_fmt);
	}
	return std_DebugList_finish(&list);
}

/* doltgres_deletes_tests_u64s_Debug_fmt is a Vec<u64>'s Debug. */
static bool
doltgres_deletes_tests_u64s_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t i = 0; i < v->len; i++)
		std_DebugList_entry(&list, &STD_VEC_AT(v, uint64, i), std_u64_Debug_fmt);
	return std_DebugList_finish(&list);
}

/*
 * test_doltgres_deletes_tests_a_simple_deletion_marks_the_rows_no_snapshot_keeps
 * checks that a simple deletion sorts its entries by tuple id, marks every
 * entry whose row no snapshot keeps, cuts the entries after the last one it
 * marked or knew, and advances the horizon over the marked rows' committed
 * writers' enders.
 */
void
test_doltgres_deletes_tests_a_simple_deletion_marks_the_rows_no_snapshot_keeps(void)
{
	static const uint64 want_tids[] = {1, 3, 0x10002, 0x20001};
	static const bool want_known[] = {true, false, true, true};
	StdVec		entries;
	DoltgresDeletesDeleting deleting;
	uint64		tid;
	uint32		horizon;
	bool		same;

	std_vec_with_capacity_Checked(&entries, 4, DOLTGRES_DELETES_DELTID);
	STD_VEC_AT(&entries, DoltgresDeletesDeltid, 0) = doltgres_deletes_tests_entry(1, 2, 0);
	STD_VEC_AT(&entries, DoltgresDeletesDeltid, 1) = doltgres_deletes_tests_entry(0, 3, 1);
	STD_VEC_AT(&entries, DoltgresDeletesDeltid, 2) = doltgres_deletes_tests_entry(0, 1, 2);
	STD_VEC_AT(&entries, DoltgresDeletesDeltid, 3) = doltgres_deletes_tests_entry(2, 1, 3);
	entries.len = 4;
	STD_VEC_AT(&entries, DoltgresDeletesDeltid, 3).known = true;
	doltgres_deletes_Deleting_new(&entries, false, 0, &deleting);
	while (doltgres_deletes_Deleting_next(&deleting, &tid))
	{
		DoltgresDeletesLooked looked = {0};

		switch (tid)
		{
			case 1:
				looked.has_row = true;
				looked.row.f0 = 100;
				looked.row.f1 = true;
				looked.row.has_f2 = true;
				looked.row.f2 = 105;
				break;
			case 3:
				looked.kept = true;
				looked.has_row = true;
				looked.row.f0 = 100;
				looked.row.f1 = true;
				break;
			case 0x10002:
				looked.has_row = true;
				looked.row.f0 = 110;
				looked.row.f1 = true;
				looked.row.has_f2 = true;
				looked.row.f2 = 110;
				break;
		}
		doltgres_deletes_Deleting_answer(&deleting, looked);
	}
	doltgres_deletes_Deleting_finish(&deleting, &entries, &horizon);
	STD_ASSERT_EQ_U64(220, 9, horizon, 105);
	same = entries.len == lengthof(want_tids);
	for (size_t i = 0; same && i < entries.len; i++)
		same = STD_VEC_AT(&entries, DoltgresDeletesDeltid, i).tid == want_tids[i] &&
			STD_VEC_AT(&entries, DoltgresDeletesDeltid, i).known == want_known[i];
	if (!same)
	{
		StdVec		want;

		std_vec_with_capacity_Checked(&want, lengthof(want_tids), DOLTGRES_DELETES_DELTID);
		for (size_t i = 0; i < lengthof(want_tids); i++)
		{
			DoltgresDeletesDeltid entry = {.tid = want_tids[i], .known = want_known[i]};

			std_vec_push_Checked(&want, &entry, DOLTGRES_DELETES_DELTID);
		}
		std_assert_failed(RUST_FILE, 222, 9, "==", &entries, doltgres_deletes_tests_marked_Debug_fmt, &want,
						  doltgres_deletes_tests_marked_Debug_fmt, NULL);
	}
	std_vec_free_Checked(&entries, DOLTGRES_DELETES_DELTID);
}

/*
 * test_doltgres_deletes_tests_a_bottom_up_deletion_visits_heaps_blocks_in_heaps_order
 * checks that a bottom-up deletion orders blocks by their promising entries,
 * bucketed, then by their count, then by block, keeps at most six blocks,
 * and stops at a block that freed nothing more.
 */
void
test_doltgres_deletes_tests_a_bottom_up_deletion_visits_heaps_blocks_in_heaps_order(void)
{
	StdVec		entries;
	StdVec		asked;
	DoltgresDeletesDeleting deleting;
	uint64		tid;
	uint32		horizon;
	size_t		kept = 0;
	bool		all = true;

	std_vec_new(&entries, DOLTGRES_DELETES_DELTID);
	for (uint64 block = 0; block < 8; block++)
		for (uint64 offset = 1; offset <= block + 1; offset++)
		{
			DoltgresDeletesDeltid made = doltgres_deletes_tests_entry(block, offset, (int16) entries.len);

			made.promising = block == 5;
			std_vec_push_Checked(&entries, &made, DOLTGRES_DELETES_DELTID);
		}
	std_vec_new(&asked, sizeof(uint64), _Alignof(uint64));
	doltgres_deletes_Deleting_new(&entries, true, 1000, &deleting);
	while (doltgres_deletes_Deleting_next(&deleting, &tid))
	{
		uint64		block = tid >> 16;
		DoltgresDeletesLooked looked = {.kept = block != 5};

		std_vec_push_Checked(&asked, &block, sizeof(uint64), _Alignof(uint64));
		doltgres_deletes_Deleting_answer(&deleting, looked);
	}
	doltgres_deletes_Deleting_finish(&deleting, &entries, &horizon);
	for (size_t i = 0; i < asked.len; i++)
		if (kept == 0 || STD_VEC_AT(&asked, uint64, kept - 1) != STD_VEC_AT(&asked, uint64, i))
			STD_VEC_AT(&asked, uint64, kept++) = STD_VEC_AT(&asked, uint64, i);
	asked.len = kept;
	if (asked.len != 2 || STD_VEC_AT(&asked, uint64, 0) != 5 || STD_VEC_AT(&asked, uint64, 1) != 4)
	{
		StdVec		want;
		uint64		five = 5;
		uint64		four = 4;

		std_vec_with_capacity_Checked(&want, 2, sizeof(uint64), _Alignof(uint64));
		std_vec_push_Checked(&want, &five, sizeof(uint64), _Alignof(uint64));
		std_vec_push_Checked(&want, &four, sizeof(uint64), _Alignof(uint64));
		std_assert_failed(RUST_FILE, 245, 9, "==", &asked, doltgres_deletes_tests_u64s_Debug_fmt, &want,
						  doltgres_deletes_tests_u64s_Debug_fmt, NULL);
	}
	for (size_t i = 0; i < entries.len; i++)
		all = all && STD_VEC_AT(&entries, DoltgresDeletesDeltid, i).tid >> 16 == 5 &&
			STD_VEC_AT(&entries, DoltgresDeletesDeltid, i).known;
	STD_ASSERT(246, 9, all, "entries.iter().all(|entry| entry.tid >> 16 == 5 && entry.known)");
	std_vec_free_Checked(&asked, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&entries, DOLTGRES_DELETES_DELTID);
}

#endif							/* DOLTGRES_TEST_DOLTGRES */
