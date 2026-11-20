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
 * The scan-key half of Postgres' nbtutils.c (access' nbtutils.rs): how a
 * btree scan's array keys are searched, set, and advanced as the scan reads
 * entries, and how an entry's values are checked against the scan's keys.
 */
#ifndef DOLTGRES_ACCESS_NBTUTILS_H
#define DOLTGRES_ACCESS_NBTUTILS_H

#ifndef FRONTEND
#include "doltgres/pg/lib.h"
#include "doltgres/std/vec.h"

/* Postgres' btree strategy numbers. */
#define ACCESS_NBTUTILS_BT_LESS_STRATEGY_NUMBER 1
#define ACCESS_NBTUTILS_BT_LESS_EQUAL_STRATEGY_NUMBER 2
#define ACCESS_NBTUTILS_BT_EQUAL_STRATEGY_NUMBER 3
#define ACCESS_NBTUTILS_BT_GREATER_EQUAL_STRATEGY_NUMBER 4
#define ACCESS_NBTUTILS_BT_GREATER_STRATEGY_NUMBER 5

/* Postgres' and nbtree's flags of a scan key. */
#define ACCESS_NBTUTILS_SK_ISNULL 0x0001
#define ACCESS_NBTUTILS_SK_ROW_HEADER 0x0004
#define ACCESS_NBTUTILS_SK_SEARCHARRAY 0x0020
#define ACCESS_NBTUTILS_SK_SEARCHNULL 0x0040
#define ACCESS_NBTUTILS_SK_SEARCHNOTNULL 0x0080
#define ACCESS_NBTUTILS_SK_BT_REQFWD 0x00010000
#define ACCESS_NBTUTILS_SK_BT_REQBKWD 0x00020000
#define ACCESS_NBTUTILS_SK_BT_SKIP 0x00040000
#define ACCESS_NBTUTILS_SK_BT_MINVAL 0x00080000
#define ACCESS_NBTUTILS_SK_BT_MAXVAL 0x00100000
#define ACCESS_NBTUTILS_SK_BT_NEXT 0x00200000
#define ACCESS_NBTUTILS_SK_BT_PRIOR 0x00400000
#define ACCESS_NBTUTILS_SK_BT_DESC 0x01000000
#define ACCESS_NBTUTILS_SK_BT_NULLS_FIRST 0x02000000

/* access_OptionU64_t is an Option<u64>: an entry's column's datum, or none for NULL. */
typedef struct access_OptionU64_t
{
	bool		has;
	uint64		value;
} access_OptionU64_t;

/* AccessNbtutilsScanDirection is ScanDirection, Postgres' ScanDirection. */
typedef enum AccessNbtutilsScanDirection
{
	ACCESS_NBTUTILS_SCAN_DIRECTION_BACKWARD,
	ACCESS_NBTUTILS_SCAN_DIRECTION_NO_MOVEMENT,
	ACCESS_NBTUTILS_SCAN_DIRECTION_FORWARD,
} AccessNbtutilsScanDirection;

/* AccessNbtutilsFunctionTag is Function's variants. */
typedef enum AccessNbtutilsFunctionTag
{
	ACCESS_NBTUTILS_FUNCTION_KEY,
	ACCESS_NBTUTILS_FUNCTION_MEMBER,
	ACCESS_NBTUTILS_FUNCTION_LOW,
	ACCESS_NBTUTILS_FUNCTION_HIGH,
	ACCESS_NBTUTILS_FUNCTION_ORDER,
	ACCESS_NBTUTILS_FUNCTION_START,
} AccessNbtutilsFunctionTag;

/*
 * AccessNbtutilsFunction is Function: one of a scan's functions Postgres
 * keeps in the scan's state, by its key or column, and for Member the
 * member's position too.
 */
typedef struct AccessNbtutilsFunction
{
	AccessNbtutilsFunctionTag tag;
	size_t		f0;
	/* f1 is Member's member position. */
	size_t		f1;
} AccessNbtutilsFunction;

/* AccessNbtutilsCallsOps is Calls: what a scan's checks ask Postgres. */
typedef struct AccessNbtutilsCallsOps
{
	bool		(*call) (const void *self, const PgCatalogCaps *caps, AccessNbtutilsFunction function,
						 uint32 collation, uint64 a, uint64 b, uint64 *result, PgPgError *error);
	bool		(*copy) (const void *self, const PgCatalogCaps *caps, size_t column, uint64 datum, uint64 *result,
						 PgPgError *error);
	void		(*free) (const void *self, size_t column, uint64 datum);
	bool		(*step) (const void *self, const PgCatalogCaps *caps, size_t array, uint64 datum, bool increment,
						 bool *has_result, uint64 *result, PgPgError *error);
} AccessNbtutilsCallsOps;

/* AccessNbtutilsCalls is a &dyn Calls: its ops and its value. */
typedef struct AccessNbtutilsCalls
{
	const AccessNbtutilsCallsOps *ops;
	const void *self;
} AccessNbtutilsCalls;

/*
 * AccessNbtutilsBtKey is BtKey: a preprocessed scan key, its flags, column,
 * strategy, argument's type and collation, its argument (none for NULL or a
 * sentinel), its function, and a row comparison's members, a Vec<BtKey>.
 */
typedef struct AccessNbtutilsBtKey
{
	uint32		flags;
	int16		attno;
	int16		strategy;
	uint32		subtype;
	uint32		collation;
	bool		has_argument;
	uint64		argument;
	AccessNbtutilsFunction func;
	StdVec		members;
} AccessNbtutilsBtKey;

/* AccessNbtutilsSkipSupport is SkipSupport: a skip array's type's lowest and highest values. */
typedef struct AccessNbtutilsSkipSupport
{
	uint64		low_elem;
	uint64		high_elem;
} AccessNbtutilsSkipSupport;

/*
 * AccessNbtutilsArrayKey is ArrayKey, Postgres' BTArrayKeyInfo: the key it
 * sets, a = ANY array's sorted elements (a Vec<u64>) with the current one,
 * or for a skip array (no elements) whether NULL is one of its values, its
 * type's skip support, and the inequalities merged into it.
 */
typedef struct AccessNbtutilsArrayKey
{
	size_t		scan_key;
	bool		has_elements;
	StdVec		elements;
	size_t		cur_elem;
	bool		null_elem;
	bool		has_sksup;
	AccessNbtutilsSkipSupport sksup;
	bool		has_low_compare;
	AccessNbtutilsBtKey low_compare;
	bool		has_high_compare;
	AccessNbtutilsBtKey high_compare;
} AccessNbtutilsArrayKey;

/*
 * AccessNbtutilsPreprocessed is Preprocessed: whether any entry can match,
 * the keys (a Vec<BtKey>), the arrays (a Vec<ArrayKey>), whether each
 * required equality key has its ordering function, and whether any array
 * is a skip array.
 */
typedef struct AccessNbtutilsPreprocessed
{
	bool		qual_ok;
	StdVec		keys;
	StdVec		arrays;
	bool		orders;
	bool		skip_scan;
} AccessNbtutilsPreprocessed;

/*
 * AccessNbtutilsChecker is Checker: the keys it checks entries against and
 * advances, the calls of their functions, the scan's direction, and whether
 * another search of the index is due.
 */
typedef struct AccessNbtutilsChecker
{
	AccessNbtutilsPreprocessed *pre;
	AccessNbtutilsCalls calls;
	AccessNbtutilsScanDirection dir;
	bool	   *need_prim_scan;
} AccessNbtutilsChecker;

extern bool access_nbtutils_ScanDirection_is_forward(AccessNbtutilsScanDirection self);
extern bool access_nbtutils_ScanDirection_is_backward(AccessNbtutilsScanDirection self);
extern AccessNbtutilsScanDirection access_nbtutils_ScanDirection_flipped(AccessNbtutilsScanDirection self);
extern void access_nbtutils_BtKey_Clone_clone(const AccessNbtutilsBtKey *self, AccessNbtutilsBtKey *result);
extern void access_nbtutils_BtKey_drop(AccessNbtutilsBtKey *self);
extern void access_nbtutils_ArrayKey_drop(AccessNbtutilsArrayKey *self);
extern void access_nbtutils_Preprocessed_drop(AccessNbtutilsPreprocessed *self);
extern int32 access_nbtutils_invert_compare_result(int32 result);
extern bool access_nbtutils_call_order(const PgCatalogCaps *caps, AccessNbtutilsCalls calls,
									   AccessNbtutilsFunction function, uint32 collation, uint64 a, uint64 b,
									   int32 *result, PgPgError *error);
extern bool access_nbtutils_compare_array_skey(const PgCatalogCaps *caps, AccessNbtutilsCalls calls, bool has_order,
											   AccessNbtutilsFunction order, access_OptionU64_t tupdatum,
											   access_OptionU64_t arrdatum, const AccessNbtutilsBtKey *cur,
											   int32 *result, PgPgError *error);
extern bool access_nbtutils_binsrch_array_skey(const PgCatalogCaps *caps, AccessNbtutilsCalls calls,
											   AccessNbtutilsFunction order, bool cur_elem_trig,
											   AccessNbtutilsScanDirection dir, access_OptionU64_t tupdatum,
											   const AccessNbtutilsArrayKey *array, const AccessNbtutilsBtKey *cur,
											   size_t *result_0, int32 *result_1, PgPgError *error);
extern bool access_nbtutils_Checker_advance_array_keys(AccessNbtutilsChecker *self, const PgCatalogCaps *caps,
													   const access_OptionU64_t *values, size_t values_len,
													   size_t sktrig, bool sktrig_required, bool *continuescan,
													   bool *result, PgPgError *error);
extern bool access_nbtutils_Checker_checkkeys(AccessNbtutilsChecker *self, const PgCatalogCaps *caps,
											  const access_OptionU64_t *values, size_t values_len, bool array_keys,
											  bool *continuescan, bool *result, PgPgError *error);
extern void access_nbtutils_Checker_start_array_keys(AccessNbtutilsChecker *self, AccessNbtutilsScanDirection dir);
#endif							/* FRONTEND */

#ifdef DOLTGRES_TEST_ACCESS
extern void test_access_nbtutils_tests_a_scan_with_no_ordering_functions_leaves_its_function_as_it_was(void);
#endif

#endif							/* DOLTGRES_ACCESS_NBTUTILS_H */
