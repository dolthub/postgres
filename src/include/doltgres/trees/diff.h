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
 * Diffs between two trees (trees' diff.rs), which skip every subtree the two
 * share, so their cost follows the size of the change, and the three-way walk
 * of the keys two trees changed from their base.
 */
#ifndef DOLTGRES_TREES_DIFF_H
#define DOLTGRES_TREES_DIFF_H

#include "doltgres/chunks/error.h"
#include "doltgres/host/io.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/str.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/cursor.h"
#include "doltgres/trees/lib.h"
#include "doltgres/trees/store.h"

/* TreesDiffChangeTag is Change's variants. */
typedef enum TreesDiffChangeTag
{
	TREES_DIFF_CHANGE_ADDED,
	TREES_DIFF_CHANGE_REMOVED,
	TREES_DIFF_CHANGE_MODIFIED,
} TreesDiffChangeTag;

/*
 * TreesDiffChange is Change: one difference between two trees' leaf entries,
 * with the addresses of the new value's out-of-line chunks when there is a new
 * value, and of the old value's in from_addresses when there is an old one;
 * each address list is a Vec<(u32, Locator)>.
 */
typedef struct TreesDiffChange
{
	TreesDiffChangeTag tag;
	union
	{
		struct
		{
			StdBytes	key;
			StdBytes	to;
			StdVec		addresses;
		}			added;
		struct
		{
			StdBytes	key;
			StdBytes	from;
			StdVec		from_addresses;
		}			removed;
		struct
		{
			StdBytes	key;
			StdBytes	from;
			StdVec		from_addresses;
			StdBytes	to;
			StdVec		addresses;
		}			modified;
	}			u;
} TreesDiffChange;

/*
 * TreesDiffDiffCursor is DiffCursor: a walk of the changes from one tree to
 * another in the trees' key order, one at a time, skipping every subtree the
 * two share.
 */
typedef struct TreesDiffDiffCursor
{
	TreesStoreNodeStore store;
	const TreesKeyOrder *order;
	TreesCursorCursor from;
	TreesCursorCursor to;

	/*
	 * at is how the keys of the current change compare: less for a removal,
	 * greater for an addition, and equal for a modification.
	 */
	StdOrdering at;
} TreesDiffDiffCursor;

/*
 * TreesDiffThreeWay is ThreeWay: a key that one side or both changed from the
 * base, each side's change from the base, none for a side that keeps the
 * base's entry.
 */
typedef struct TreesDiffThreeWay
{
	bool		has_ours;
	TreesDiffChange ours;
	bool		has_theirs;
	TreesDiffChange theirs;
} TreesDiffThreeWay;

/*
 * TreesDiffThreeWayCursor is ThreeWayCursor: a walk of the keys two trees
 * changed from their base in the trees' key order, one at a time, from the two
 * diffs from the base.
 */
typedef struct TreesDiffThreeWayCursor
{
	const TreesKeyOrder *order;
	TreesDiffDiffCursor ours;
	TreesDiffDiffCursor theirs;

	/*
	 * at is how the keys of the two diffs' current changes compare, less when
	 * only ours has one left and greater when only theirs has.
	 */
	StdOrdering at;
} TreesDiffThreeWayCursor;

/* pin's instance at Result<bool>, which diff.c defines. */
#define TREES_PIN_SCOPE extern
#define TREES_PIN_DECLARE
#define TREES_PIN_RS ResultBool
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , bool *result, ChunksErrorError *error
#include "doltgres/trees/pin_template.h"

extern bool trees_diff_SliceU8_PartialEqSliceU8_eq(const StdBytes *self, const StdBytes *other);
extern void trees_diff_Change_Clone_clone(const void *self_, void *result_);
extern bool trees_diff_Change_Debug_fmt(const void *value, StdFormatter *f);
extern bool trees_diff_Change_PartialEq_eq(const void *self_, const void *other_);
extern void trees_diff_Change_drop(void *self_);
extern StdBytes trees_diff_Change_key(const TreesDiffChange *self);
extern StdBytes trees_diff_Change_from(const TreesDiffChange *self);
extern StdBytes trees_diff_Change_to(const TreesDiffChange *self);
extern bool trees_diff_diff(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed from, TreesStoreNamed to,
							const TreesKeyOrder *order,
							bool (*f) (void *captures, HostIoIo *io, TreesDiffChange *change, bool *result,
									   ChunksErrorError *error),
							void *f_captures, void (*f_drop) (void *captures), ChunksErrorError *error);
extern bool trees_diff_DiffCursor_new(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed from,
									  TreesStoreNamed to, const TreesKeyOrder *order, TreesDiffDiffCursor *result,
									  ChunksErrorError *error);
extern void trees_diff_DiffCursor_change(const TreesDiffDiffCursor *self, bool *some, TreesDiffChange *result);
extern bool trees_diff_DiffCursor_advance(TreesDiffDiffCursor *self, HostIoIo *io, ChunksErrorError *error);
extern void trees_diff_DiffCursor_drop(TreesDiffDiffCursor *self);
extern StdBytes trees_diff_ThreeWay_key(const TreesDiffThreeWay *self);
extern StdBytes trees_diff_ThreeWay_base(const TreesDiffThreeWay *self);
extern StdBytes trees_diff_ThreeWay_ours(const TreesDiffThreeWay *self);
extern StdBytes trees_diff_ThreeWay_theirs(const TreesDiffThreeWay *self);
extern void trees_diff_ThreeWay_drop(TreesDiffThreeWay *self);
extern bool trees_diff_ThreeWayCursor_new(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed base,
										  TreesStoreNamed ours, TreesStoreNamed theirs, const TreesKeyOrder *order,
										  TreesDiffThreeWayCursor *result, ChunksErrorError *error);
extern void trees_diff_ThreeWayCursor_change(const TreesDiffThreeWayCursor *self, bool *some,
											 TreesDiffThreeWay *result);
extern bool trees_diff_ThreeWayCursor_advance(TreesDiffThreeWayCursor *self, HostIoIo *io, ChunksErrorError *error);
extern void trees_diff_ThreeWayCursor_drop(TreesDiffThreeWayCursor *self);

#endif							/* DOLTGRES_TREES_DIFF_H */
