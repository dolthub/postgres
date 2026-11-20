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
 * Scans of a btree's entries by scan keys (access' nbtsearch.rs), Postgres'
 * nbtsearch.c, over a tree whose keys are the key encodings of the index's
 * columns, each cut at the tree's bound: each search of the index starts
 * where the scan's keys and its arrays' current elements put it.
 */
#ifndef DOLTGRES_ACCESS_NBTSEARCH_H
#define DOLTGRES_ACCESS_NBTSEARCH_H

#ifndef FRONTEND
#include "doltgres/access/key.h"
#include "doltgres/access/nbtutils.h"
#include "doltgres/pg/lib.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/vec.h"

/*
 * AccessNbtsearchKeyColumn is KeyColumn: a key column of a btree as its
 * scans read it, its keys' order and key type, its collation, and its type's
 * stored length and whether it is passed by value.
 */
typedef struct AccessNbtsearchKeyColumn
{
	EncodingKeyOrder order;
	const AccessKeyKeyType *key_type;
	uint32		collation;
	int16		len;
	bool		by_value;
} AccessNbtsearchKeyColumn;

/* access_BoundVecU8_t is an ops::Bound<Vec<u8>>: a range's end, included, excluded, or unbounded, and its key. */
typedef struct access_BoundVecU8_t
{
	StdBoundTag tag;
	StdVec		key;
} access_BoundVecU8_t;

/* AccessNbtsearchKeyRange is KeyRange: a range of a tree's keys, its lower and upper bounds. */
typedef struct AccessNbtsearchKeyRange
{
	access_BoundVecU8_t f0;
	access_BoundVecU8_t f1;
} AccessNbtsearchKeyRange;

/* AccessNbtsearchPosition is Position: where an entry is among a search's, its key and the tuple id of its copy. */
typedef struct AccessNbtsearchPosition
{
	StdVec		key;
	uint64		copy;
} AccessNbtsearchPosition;

/*
 * AccessNbtsearchReaderOps is Reader: a search between bounds in a
 * direction, past a position when it resumes from one; its entries'
 * positions in turn; and the values (a Vec<Option<Datum>>) the entry it gave
 * last decodes to.
 */
typedef struct AccessNbtsearchReaderOps
{
	bool		(*search) (void *self, const PgCatalogCaps *caps, const access_BoundVecU8_t *lower,
						   const access_BoundVecU8_t *upper, bool backward, const AccessNbtsearchPosition *after,
						   PgPgError *error);
	bool		(*next) (void *self, const PgCatalogCaps *caps, bool *has_result, AccessNbtsearchPosition *result,
						 PgPgError *error);
	bool		(*values) (void *self, const PgCatalogCaps *caps, StdVec *result, PgPgError *error);
} AccessNbtsearchReaderOps;

/* AccessNbtsearchReader is a &mut dyn Reader: its ops and its value. */
typedef struct AccessNbtsearchReader
{
	const AccessNbtsearchReaderOps *ops;
	void	   *self;
} AccessNbtsearchReader;

/*
 * AccessNbtsearchSearchCallsOps is SearchCalls: its supertraits Calls and
 * Names, a column's start ordering function for an argument's type, the
 * stored bytes of an argument passed by reference, and the keeping of a
 * comparator column's argument past the bound under its name.
 */
typedef struct AccessNbtsearchSearchCallsOps
{
	AccessNbtutilsCallsOps calls;
	AccessKeyNamesOps names;
	bool		(*start_order) (const void *self, const PgCatalogCaps *caps, size_t column, uint32 subtype,
								PgPgError *error);
	bool		(*bytes) (const void *self, const PgCatalogCaps *caps, uint64 argument, int16 len, StdVec *result,
						  PgPgError *error);
	bool		(*probe) (const void *self, const PgCatalogCaps *caps, const uint8 name[32], StdVec *stored,
						  PgPgError *error);
} AccessNbtsearchSearchCallsOps;

/* AccessNbtsearchSearchCalls is a &dyn SearchCalls: its ops and its value. */
typedef struct AccessNbtsearchSearchCalls
{
	const AccessNbtsearchSearchCallsOps *ops;
	const void *self;
} AccessNbtsearchSearchCalls;

/* access_TupleBtKeyOptionFunction_t is a start key with its ordering function, a (BtKey, Option<Function>). */
typedef struct access_TupleBtKeyOptionFunction_t
{
	AccessNbtutilsBtKey f0;
	bool		has_f1;
	AccessNbtutilsFunction f1;
} access_TupleBtKeyOptionFunction_t;

/* access_OptionU32_t is an Option<u32>: the argument type a column's start ordering function is ready for, or none. */
typedef struct access_OptionU32_t
{
	bool		has;
	uint32		value;
} access_OptionU32_t;

/*
 * AccessNbtsearchStart is Start: where a search starts when its seek could
 * encode only some of its start keys, the start keys with their ordering
 * functions (a Vec<(BtKey, Option<Function>)>), and whether the search
 * starts past the entries equal to them.
 */
typedef struct AccessNbtsearchStart
{
	StdVec		keys;
	bool		nextkey;
} AccessNbtsearchStart;

/*
 * AccessNbtsearchBtScan is BtScan: a scan of a btree's entries by scan
 * keys, Postgres' BTScanOpaqueData.
 */
typedef struct AccessNbtsearchBtScan
{
	size_t		bound;

	/*
	 * compared is whether the tree is ordered by a comparator, a key column
	 * being compared through its class.
	 */
	bool		compared;
	AccessNbtutilsPreprocessed pre;
	/* searching is whether the scan reads a search of the index now. */
	bool		searching;
	bool		backward;

	/*
	 * need_prim_scan is whether the scan has to search the index again for
	 * its arrays' new elements.
	 */
	bool		need_prim_scan;

	/*
	 * started is whether the scan began, its arrays set to their first
	 * elements.
	 */
	bool		started;
	/* ended is whether the scan read its last entry in its direction. */
	bool		ended;
	/* current is the position of the entry returned last, Postgres' currPos. */
	bool		has_current;
	AccessNbtsearchPosition current;

	/*
	 * resume_after is the position after which the scan goes on without a
	 * search of its own.
	 */
	bool		has_resume_after;
	AccessNbtsearchPosition resume_after;
	/* nsearches is the searches of the index the scan began. */
	uint64		nsearches;

	/*
	 * start is where the current search starts, when its seek reads entries
	 * before that.
	 */
	bool		has_start;
	AccessNbtsearchStart start;

	/*
	 * starts is the start ordering function each column has ready, by its
	 * argument type (a Vec<Option<u32>>).
	 */
	StdVec		starts;

	/*
	 * chunk is the keys of the tree the scan reads, the unit a parallel
	 * scan's worker took last.
	 */
	bool		has_chunk;
	AccessNbtsearchKeyRange chunk;
} AccessNbtsearchBtScan;

/* AccessNbtsearchBtMark is BtMark: a marked position of a btree scan, Postgres' markPos. */
typedef struct AccessNbtsearchBtMark
{
	AccessNbtsearchPosition position;
} AccessNbtsearchBtMark;

extern access_BoundVecU8_t access_nbtsearch_tighter(access_BoundVecU8_t *a, access_BoundVecU8_t *b, StdOrdering side);
extern bool access_nbtsearch_is_empty(const access_BoundVecU8_t *lower, const access_BoundVecU8_t *upper);
extern void access_nbtsearch_Bound_drop(access_BoundVecU8_t *self);
extern void access_nbtsearch_Bound_Clone_clone(const access_BoundVecU8_t *self, access_BoundVecU8_t *result);
extern void access_nbtsearch_KeyRange_drop(AccessNbtsearchKeyRange *self);
extern void access_nbtsearch_Position_Clone_clone(const AccessNbtsearchPosition *self, AccessNbtsearchPosition *result);
extern void access_nbtsearch_Position_drop(AccessNbtsearchPosition *self);
extern bool access_nbtsearch_past(const uint8 *prefix, size_t prefix_len, bool compared, StdVec *result);
extern void access_nbtsearch_BtScan_new(const AccessNbtsearchKeyColumn *columns, size_t columns_len, size_t bound,
										AccessNbtutilsPreprocessed *pre, bool backward,
										AccessNbtsearchBtScan *result);
extern void access_nbtsearch_BtScan_drop(AccessNbtsearchBtScan *self);
extern bool access_nbtsearch_BtScan_next_entry(AccessNbtsearchBtScan *self, const PgCatalogCaps *caps,
											   const AccessNbtsearchKeyColumn *columns, size_t columns_len,
											   AccessNbtsearchReader reader, AccessNbtsearchSearchCalls calls,
											   bool *result, PgPgError *error);
extern bool access_nbtsearch_BtScan_set_direction(AccessNbtsearchBtScan *self, bool backward, bool *result);
extern void access_nbtsearch_BtScan_restrict(AccessNbtsearchBtScan *self, AccessNbtsearchKeyRange *chunk);
extern uint64 access_nbtsearch_BtScan_nsearches(const AccessNbtsearchBtScan *self);
extern bool access_nbtsearch_BtScan_markpos(const AccessNbtsearchBtScan *self, AccessNbtsearchBtMark *result);
extern void access_nbtsearch_BtScan_restrpos(AccessNbtsearchBtScan *self, AccessNbtsearchSearchCalls calls,
											 const AccessNbtsearchBtMark *mark);
extern void access_nbtsearch_BtMark_Clone_clone(const AccessNbtsearchBtMark *self, AccessNbtsearchBtMark *result);
extern void access_nbtsearch_BtMark_drop(AccessNbtsearchBtMark *self);
#endif

#endif							/* DOLTGRES_ACCESS_NBTSEARCH_H */
