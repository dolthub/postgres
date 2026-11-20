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
 * The copy of a table CLUSTER and VACUUM FULL make (doltgres' rewrite.rs):
 * every row version of the old storage judged, ordered, and given its new
 * tuple id as heap's rewrite gives its tuples theirs; and the versions ALTER
 * TABLE SET TABLESPACE's move carries as they are.
 */
#ifndef DOLTGRES_DOLTGRES_REWRITE_H
#define DOLTGRES_DOLTGRES_REWRITE_H

#ifndef FRONTEND
#include "doltgres/access/heaptuple.h"
#include "doltgres/doltgres/scan.h"
#include "doltgres/host/shared.h"
#include "doltgres/pg/lib.h"
#include "doltgres/repo/lib.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/record.h"
#include "doltgres/txn/rewrite.h"
#include "doltgres/txn/visible.h"

/* DoltgresRewriteFromTag is From's variants. */
typedef enum DoltgresRewriteFromTag
{
	DOLTGRES_REWRITE_FROM_BASE,
	DOLTGRES_REWRITE_FROM_DELTA,
} DoltgresRewriteFromTag;

/*
 * DoltgresRewriteFrom is From: where a row version's row is read from, its
 * key's base entry by the entry's place, or one of its key's versions by its
 * place among them.
 */
typedef struct DoltgresRewriteFrom
{
	DoltgresRewriteFromTag tag;
	size_t		place;
} DoltgresRewriteFrom;

/*
 * DoltgresRewriteOld is Old: a row version of the old storage, its key's
 * place, its tuple id, its writer's own xid, heap's HeapTupleSatisfiesVacuum
 * answer, its ender's place among its key's versions, the xid of an ender
 * that updated it with the tuple id of the row it updated it to, and where
 * its row is.
 */
typedef struct DoltgresRewriteOld
{
	size_t		key;
	uint64		tuple;
	uint64		xmin;
	TxnVisibleVacuumed vacuumed;
	bool		has_ender;
	size_t		ender;
	bool		has_update;
	uint64		update_xmax;
	uint64		update_next;
	DoltgresRewriteFrom from;
} DoltgresRewriteOld;

/* doltgres_TupleVecU8Bool_t is a (Vec<u8>, bool): a version's record and whether its writer aborted. */
typedef struct doltgres_TupleVecU8Bool_t
{
	StdVec		f0;
	bool		f1;
} doltgres_TupleVecU8Bool_t;

/*
 * DoltgresRewriteKey is Key: a key of the old storage, its bytes, and the
 * record of each of its versions the base doesn't apply, newest first, with
 * whether its writer aborted.
 */
typedef struct DoltgresRewriteKey
{
	StdVec		key;
	StdVec		versions;
} DoltgresRewriteKey;

/*
 * DoltgresRewriteRead is Read: the old storage as the rewrite reads it, the
 * base it copies, the horizon of the rows it leaves out, that base's
 * entries, the keys, and every row version.
 */
typedef struct DoltgresRewriteRead
{
	TxnBaseBase base;
	uint64		left_out;
	StdVec		based;
	StdVec		keys;
	StdVec		olds;
} DoltgresRewriteRead;

/*
 * DoltgresRewriteTable is Table: the old table as the rewrite stores its rows
 * into a slot and forms them again, its relation, its keyed layout, whether
 * it is keyless, and its TOAST target.
 */
typedef struct DoltgresRewriteTable
{
	const PgRel *rel;
	const uint8 *keyed;
	size_t		keyed_len;
	bool		keyless;
	size_t		target;
} DoltgresRewriteTable;

/*
 * DoltgresRewriteReformer is Reformer: what re-forms the old storage's rows,
 * its repository and reader, the places of the key columns rows are formed
 * under, the slot rows are stored into, the old table, the old storage as
 * read, what gives each old row the transform its read takes, and the layout
 * rows are formed by, or until the first row's columns make it, its key's
 * columns.
 */
typedef struct DoltgresRewriteReformer
{
	const RepoRepo *repo;
	const HostSharedReader *reading;
	StdVec		keyed;
	PgOwnedSlot slot;
	const DoltgresRewriteTable *table;
	const DoltgresRewriteRead *read;
	DoltgresScanEpochs epochs;
	bool		has_layout;
	AccessHeaptupleRowLayout layout;
	StdVec		key_columns;
} DoltgresRewriteReformer;

/*
 * DoltgresRewriteCounts is Counts: what heap's rewrite answers of the row
 * versions it read, those it kept, those dead to every snapshot, and those
 * ended or being ended but not dead to every snapshot.
 */
typedef struct DoltgresRewriteCounts
{
	double		kept;
	double		vacuumed;
	double		recently_dead;
} DoltgresRewriteCounts;

/* doltgres_OptionU64_t is an Option<u64>: a row version's new tuple id, none for one the rewrite dropped. */
typedef struct doltgres_OptionU64_t
{
	bool		has;
	uint64		f0;
} doltgres_OptionU64_t;

/* doltgres_OptionFormed_t is an Option<Formed>: a row version's row re-formed, none when the stored one stands. */
typedef struct doltgres_OptionFormed_t
{
	bool		has;
	AccessHeaptupleFormed f0;
} doltgres_OptionFormed_t;

/* doltgres_OptionVecU32_t is an Option<Vec<u32>>: a re-formed row's out-of-line values' ids, none when they stand. */
typedef struct doltgres_OptionVecU32_t
{
	bool		has;
	StdVec		f0;
} doltgres_OptionVecU32_t;

/*
 * DoltgresRewriteRewritten is Rewritten: what the rewrite gave the old
 * storage's row versions, each one's new tuple id, the heap bytes it packed
 * it with, its row re-formed when that differs from the row stored, its
 * out-of-line values' ids when a value moved out of line anew, each value
 * id's map entry and how many name it (a Vec sorted by id, as its BTreeMap
 * iterates), its counts, and where its packing ended.
 */
typedef struct DoltgresRewriteRewritten
{
	StdVec		new_;
	StdVec		bytes;
	StdVec		formed;
	StdVec		ids;
	StdVec		values;
	DoltgresRewriteCounts counts;
	TxnRewritePacked packed;
} DoltgresRewriteRewritten;

/*
 * DoltgresRewriteCarry is Carry: a version the new storage carries, its row
 * change, the command it showed, and its row lock.
 */
typedef struct DoltgresRewriteCarry
{
	TxnRecordRowChange f0;
	TxnMemtableVersionShown f1;
	TxnMemtableLockMode f2;
} DoltgresRewriteCarry;
extern void doltgres_rewrite_Read_drop(DoltgresRewriteRead *self);
extern bool doltgres_rewrite_read(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
								  TxnBaseBase *base, uint64 horizon, DoltgresRewriteRead *result, HostError *error);
extern void doltgres_rewrite_Rewritten_drop(DoltgresRewriteRewritten *self);
extern bool doltgres_rewrite_rewritten(const PgCatalogCaps *caps, const RepoRepo *repo, const HostSharedReader *reading,
									   const DoltgresRewriteTable *table, StdVec *key_columns, const PgRel *new_table,
									   const PgRel *index, bool sorted, const DoltgresRewriteRead *read, uint64 horizon,
									   DoltgresRewriteRewritten *result, PgPgError *error);
extern void doltgres_rewrite_Reformer_drop(DoltgresRewriteReformer *self);
extern bool doltgres_rewrite_Reformer_new(const PgMayRaise *raise, const RepoRepo *repo, const HostSharedReader *reading,
										  const DoltgresRewriteTable *table, const DoltgresRewriteRead *read,
										  StdVec *key_columns, DoltgresRewriteReformer *result, PgPgError *error);
extern bool doltgres_rewrite_Reformer_reformed(DoltgresRewriteReformer *self, const PgCatalogCaps *caps, size_t at,
											   StdVec *values, DoltgresScanRow *result_0,
											   AccessHeaptupleFormed *result_1, bool *has_result_2, StdVec *result_2,
											   PgPgError *error);
extern bool doltgres_rewrite_Reformer_null_key(DoltgresRewriteReformer *self, size_t *result);
extern const PgRel *doltgres_rewrite_Reformer_rel(const DoltgresRewriteReformer *self);
extern void doltgres_rewrite_copied(const DoltgresRewriteRead *read, DoltgresRewriteRewritten *done, StdVec *result);
extern bool doltgres_rewrite_carrying(const DoltgresRewriteRead *read, const DoltgresRewriteRewritten *done,
									  bool keyless, HostStorage storage, uint32 table, const TxnRecordToast *toast,
									  StdVec *result, HostError *error);
extern bool doltgres_rewrite_moving(HostIoIo *io, const HostSharedHeld *held, uint64 folded, HostStorage storage,
									uint32 table, bool has_index, uint32 index, StdVec *result, HostError *error);
#endif

#endif							/* DOLTGRES_DOLTGRES_REWRITE_H */
