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
 * A table's key and row layout from the catalog (doltgres' layout.rs): the
 * key its primary key gives a storage, each column's key type, the order of
 * its tree, and a slot's row as a row's forming takes it.
 */
#ifndef DOLTGRES_DOLTGRES_LAYOUT_H
#define DOLTGRES_DOLTGRES_LAYOUT_H

#ifndef FRONTEND
#include "doltgres/access/heaptuple.h"
#include "doltgres/access/key.h"
#include "doltgres/access/slot.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/lock.h"
#include "doltgres/pg/lib.h"
#include "doltgres/trees/lib.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/insert.h"

/*
 * doltgres_TupleOrderKeyTypeOptionCompared_t is a Compared's column, an
 * (Order, KeyType, Option<host::Compared>): its order, its key type, and for
 * a column compared through its class how its function is called.
 */
typedef struct doltgres_TupleOrderKeyTypeOptionCompared_t
{
	EncodingKeyOrder f0;
	AccessKeyKeyType f1;
	bool		has_f2;
	HostCompared f2;
} doltgres_TupleOrderKeyTypeOptionCompared_t;

/* doltgres_TupleOrderKeyTypeU32_t is an (Order, KeyType, u32): a comparator column's order, key type, and collation. */
typedef struct doltgres_TupleOrderKeyTypeU32_t
{
	EncodingKeyOrder f0;
	AccessKeyKeyType f1;
	uint32		f2;
} doltgres_TupleOrderKeyTypeU32_t;

/* DoltgresLayoutHeld is Held: a slot's column value, none for a null or dropped one, a word, or a value's bytes. */
typedef struct DoltgresLayoutHeld
{
	bool		has;
	uint64		f0;
	StdVec		f1;
} DoltgresLayoutHeld;

/*
 * DoltgresLayoutProbe is an entry of Probes' map: a scan's own long value's
 * name, which its key gives it, and the value's bytes (a Vec<u8>).
 */
typedef struct DoltgresLayoutProbe
{
	uint8		name[32];
	StdVec		bytes;
} DoltgresLayoutProbe;

extern void doltgres_layout_VecProbe_drop(void *probes);

/*
 * Probes is IoMutex<BTreeMap<[u8; 32], Vec<u8>>>: a scan's own long values of
 * comparator columns past its tree's bound, the map a Vec<Probe> sorted by
 * name, as the port's internals may hold it.
 */
#define HOST_LOCK_SCOPE extern
#define HOST_LOCK_DECLARE
#define HOST_LOCK_TAKEN_TS VecProbe
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) std_vec_new((place), sizeof(DoltgresLayoutProbe), _Alignof(DoltgresLayoutProbe))
#define HOST_LOCK_TAKEN_DROP(place) doltgres_layout_VecProbe_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_LAYOUT_PROBES_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecProbe
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) std_vec_new((place), sizeof(DoltgresLayoutProbe), _Alignof(DoltgresLayoutProbe))
#include "doltgres/host/lock_template.h"

/* DOLTGRES_LAYOUT_BLOB_NAMES_NAMES_OPS is BlobNames' Names: a long value's name, its blob's root. */
extern const AccessKeyNamesOps DOLTGRES_LAYOUT_BLOB_NAMES_NAMES_OPS;

extern bool doltgres_layout_catalog_key(const PgMayRaise *raise, const PgCatalog *catalog, const PgRel *rel,
										TxnBaseKey *result, PgPgError *error);
extern bool doltgres_layout_key_columns(const PgMayRaise *raise, const PgCatalog *catalog, const TxnBaseKey *key,
										StdVec *result, PgPgError *error);
extern void doltgres_layout_VecKeyed_drop(void *columns);
extern bool doltgres_layout_same_key(const PgMayRaise *raise, const PgCatalog *catalog, const TxnBaseKey *a,
									 const TxnBaseKey *b, bool *result, PgPgError *error);
extern bool doltgres_layout_storage(const PgMayRaise *raise, const PgCatalog *catalog, uint32 typid, int16 *result_0,
									bool *result_1, uint8 *result_2, PgPgError *error);
extern bool doltgres_layout_key_type(const PgMayRaise *raise, const PgCatalog *catalog, uint32 typid, uint32 collid,
									 AccessKeyKeyType *result, PgPgError *error);
extern bool doltgres_layout_key_order(const PgMayRaise *raise, const PgCatalog *catalog, const TxnBaseKey *key,
									  StdVec *columns, TreesKeyOrder *result, PgPgError *error);
extern bool doltgres_layout_class_key_type(const PgMayRaise *raise, const PgCatalog *catalog, uint32 typid,
										   uint32 collation, bool default_class, AccessKeyKeyType *result,
										   PgPgError *error);
extern bool doltgres_layout_compared_columns(const PgMayRaise *raise, const PgCatalog *catalog, StdVec *columns,
											 bool (*function) (const void *captures, const PgMayRaise *raise,
															   const PgCatalog *catalog, size_t at, uint32 *result,
															   PgPgError *error),
											 const void *function_captures, StdVec *result, PgPgError *error);
extern void doltgres_layout_index_order(StdArcInner *columns, StdArcInner *probes, TreesKeyOrder *result);
extern void doltgres_layout_Compared_drop(void *compared);
extern void doltgres_layout_Probes_drop(void *probes);
extern bool doltgres_layout_equal(const TreesKeyOrder *order, TxnInsertEqual *result);
extern void doltgres_layout_VecHeld_drop(void *held);
extern bool doltgres_layout_slot_row(const PgMayRaise *raise, const PgCatalog *catalog, const PgRel *rel,
									 const PgSlot *source, StdVec *result_0, StdVec *result_1, PgPgError *error);
extern void doltgres_layout_values(const AccessHeaptupleColumn *columns, size_t columns_len,
								   const DoltgresLayoutHeld *held, size_t held_len, StdVec *result);

#ifdef DOLTGRES_TEST_DOLTGRES
extern void test_doltgres_layout_tests_a_comparator_orders_compared_columns_by_their_function(void);
#endif
#endif

#endif							/* DOLTGRES_DOLTGRES_LAYOUT_H */
