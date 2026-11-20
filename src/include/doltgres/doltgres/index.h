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
 * Which btree indexes the fork stores (doltgres' index.rs), and an index's
 * description, from which an entry is formed from the executor's values.
 */
#ifndef DOLTGRES_DOLTGRES_INDEX_H
#define DOLTGRES_DOLTGRES_INDEX_H

#ifndef FRONTEND
#include "doltgres/access/index.h"
#include "doltgres/access/nbtsearch.h"
#include "doltgres/doltgres/layout.h"
#include "doltgres/host/io.h"
#include "doltgres/host/owned.h"
#include "doltgres/pg/lib.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/memtable/version.h"
#include "pg_calls.h"

/* doltgres_TupleBoolI16_t is a column's stored form, a (bool, i16): whether it is passed by value, and its length. */
typedef struct doltgres_TupleBoolI16_t
{
	bool		f0;
	int16		f1;
} doltgres_TupleBoolI16_t;

/*
 * DoltgresIndexDescribed is Described: an index's description, how its
 * entries are made, each of its columns' form (a Vec<(bool, i16)>), its key
 * columns first, how many are keys, and whether its table is keyless.
 */
typedef struct DoltgresIndexDescribed
{
	AccessIndexIndexKey key;
	StdVec		forms;
	size_t		keys;
	bool		keyless;

	/*
	 * keyed is whether the index has no tree of its own, its entries its
	 * table's rows.
	 */
	bool		keyed;

	/*
	 * unique is whether the index is unique and checked immediately, so equal
	 * keys of two rows are one entry key.
	 */
	bool		unique;

	/*
	 * collations is the key columns' collations (a Vec<u32>), which a scan's
	 * keys must share to bound them.
	 */
	StdVec		collations;

	/*
	 * compared is the key columns as the tree's comparator takes them (an
	 * Arc<Compared>), or NULL for a tree ordered by bytes.
	 */
	StdArcInner *compared;

	/*
	 * records is whether the index keeps a record of each entry, since
	 * forming its entries runs user code.
	 */
	bool		records;
} DoltgresIndexDescribed;

/*
 * DoltgresIndexFormed is Formed: a row's entry as its index's description
 * forms it, its key and the placing of its value.
 */
typedef struct DoltgresIndexFormed
{
	AccessIndexEntryKey key;
	AccessHeaptuplePlacing placing;
} DoltgresIndexFormed;

/*
 * DoltgresIndexInserting is Inserting: an insert's description of its index
 * (an Arc<Described>), named by its handle.
 */
typedef struct DoltgresIndexInserting
{
	StdArcInner *f0;
} DoltgresIndexInserting;

/* doltgres_TupleU32Described_t is an index of a removal, a (u32, Described): its object id and its description. */
typedef struct doltgres_TupleU32Described_t
{
	uint32		f0;
	DoltgresIndexDescribed f1;
} doltgres_TupleU32Described_t;

/*
 * DoltgresIndexOldEntries is OldEntries: a table's removal of old entries,
 * the slot of the binding's table that holds its Postgres side, and each of
 * its indexes' object id and description (an Arc<Vec<(u32, Described)>>).
 */
typedef struct DoltgresIndexOldEntries
{
	int32		slot;
	StdArcInner *indexes;
} DoltgresIndexOldEntries;

/*
 * DoltgresIndexListed is Listed: a table's removal in the backend's list,
 * the table's and its indexes' object ids (a Vec<u32>), the removal's slot
 * and handle, and whether an invalidation made it stale.
 */
typedef struct DoltgresIndexListed
{
	uint32		table;
	StdVec		indexes;
	int32		slot;
	HostOwnedHandle handle;
	bool		stale;
} DoltgresIndexListed;

/*
 * doltgres_TupleU32U64U64U32_t is Kept: an index whose entry an update kept,
 * the update's new row's tuple id, and the writer's xid and command.
 */
typedef struct doltgres_TupleU32U64U64U32_t
{
	uint32		f0;
	uint64		f1;
	uint64		f2;
	uint32		f3;
} doltgres_TupleU32U64U64U32_t;

/* doltgres_TupleI32Arc_t is a removal as opened lends it, an (i32, Arc<Vec<(u32, Described)>>). */
typedef struct doltgres_TupleI32Arc_t
{
	int32		f0;
	StdArcInner *f1;
} doltgres_TupleI32Arc_t;

/* DoltgresIndexOldTag is Old's variants. */
typedef enum DoltgresIndexOldTag
{
	DOLTGRES_INDEX_OLD_KEY,
	DOLTGRES_INDEX_OLD_RECORDED,
} DoltgresIndexOldTag;

/* DoltgresIndexOld is Old: a removed row's entry of an index, its key (a Vec<u8>), or the row's record. */
typedef struct DoltgresIndexOld
{
	DoltgresIndexOldTag tag;
	StdVec		key;
} DoltgresIndexOld;

/*
 * DoltgresIndexRemoving is Removing: what a removal of old entries takes of
 * the row a table's change ends, its slot, tuple id, and key, and of an
 * update's new row the same.
 */
typedef struct DoltgresIndexRemoving
{
	PgSlot	   *old_slot;
	uint64		old_tuple;
	StdBytes	old_key;
	bool		has_new;
	PgSlot	   *new_slot;
	uint64		new_tuple;
	StdBytes	new_key;
} DoltgresIndexRemoving;

/* DoltgresIndexRemove is removed's remove: an index's old entry removed, which it takes. */
typedef bool (*DoltgresIndexRemove) (void *captures, const PgCatalogCaps *caps, HostIoIo *io, const PgRel *index,
									 DoltgresIndexOld *old, const DoltgresIndexDescribed *described,
									 PgPgError *error);

/* DoltgresIndexWork is opened's work over a removal's opened indexes, with its slot and its indexes. */
typedef bool (*DoltgresIndexWork) (void *captures, const PgCatalogCaps *caps, HostIoIo *io, int32 slot,
								   const doltgres_TupleU32Described_t *indexes, size_t indexes_len,
								   PgPgError *error);

extern HostOwnedTable DOLTGRES_INDEX_INSERTING;
extern HostOwnedTable DOLTGRES_INDEX_OLD_ENTRIES;

/* doltgres_index_Inserting_OWNED_OPS is Inserting's Owned: its kind, its table, and its release. */
extern const HostOwnedOwnedOps doltgres_index_Inserting_OWNED_OPS;

/* doltgres_index_OldEntries_OWNED_OPS is OldEntries' Owned: its kind, its table, and its release. */
extern const HostOwnedOwnedOps doltgres_index_OldEntries_OWNED_OPS;

extern bool doltgres_index_ported(const PgMayRaise *raise, const PgCatalog *catalog, const PgRel *rel, bool *result,
								  PgPgError *error);
extern bool doltgres_index_describe(const PgMayRaise *raise, const PgCatalog *catalog, const PgRel *index,
									const access_TupleUsizeOrderKeyType_t *table_key, size_t table_key_len,
									const TxnBaseKey *recorded, DoltgresIndexDescribed *result, PgPgError *error);
extern void doltgres_index_Described_drop(DoltgresIndexDescribed *self);
extern bool doltgres_index_keyed(const PgMayRaise *raise, const PgCatalog *catalog, const PgRel *index,
								 const TxnBaseKey *recorded, bool *result, PgPgError *error);
extern bool doltgres_index_Described_keyless(const DoltgresIndexDescribed *self);
extern bool doltgres_index_Described_keyed(const DoltgresIndexDescribed *self);
extern bool doltgres_index_Described_unique(const DoltgresIndexDescribed *self);
extern bool doltgres_index_Described_records(const DoltgresIndexDescribed *self);
extern bool doltgres_index_Described_null_key(const DoltgresIndexDescribed *self, const access_OptionVecU8_t *read,
											  size_t read_len);
extern const doltgres_TupleBoolI16_t *doltgres_index_Described_forms(const DoltgresIndexDescribed *self,
																	 size_t *result_len);
extern size_t doltgres_index_Described_scan_bound(const DoltgresIndexDescribed *self);
extern void doltgres_index_Described_order(const DoltgresIndexDescribed *self, StdArcInner *probes,
										   TreesKeyOrder *result);
extern bool doltgres_index_Described_compared(const DoltgresIndexDescribed *self);
extern const AccessIndexIndexKey *doltgres_index_Described_entry_key(const DoltgresIndexDescribed *self);
extern void doltgres_index_Described_scan_columns(const DoltgresIndexDescribed *self, StdVec *result);
extern bool doltgres_index_Described_values(const DoltgresIndexDescribed *self, const PgCatalogCaps *caps,
											const PgDatums *values, const PgIsNull *isnull, StdVec *result,
											PgPgError *error);
extern void doltgres_index_values_drop(StdVec *values);
extern bool doltgres_index_Described_key(const DoltgresIndexDescribed *self, AccessKeySortKeys sorter,
										 const access_OptionVecU8_t *read, size_t read_len, const uint8 *row_key,
										 size_t row_key_len, AccessIndexEntryKey *result, PgPgError *error);
extern bool doltgres_index_Described_formed(const DoltgresIndexDescribed *self, AccessKeySortKeys sorter,
											const access_OptionVecU8_t *read, size_t read_len, const uint8 *row_key,
											size_t row_key_len, DoltgresIndexFormed *result, PgPgError *error);
extern bool doltgres_index_inserting(HostIoIo *io, HostOwnedHandle handle, StdArcInner **result, HostError *error);
extern bool doltgres_index_unchanged(HostIoIo *io, uint32 index, uint64 tuple, const TxnMemtableVersionWriter *writer);
extern void doltgres_index_invalidated(uint32 relid);
extern bool doltgres_index_ended(HostIoIo *io, HostError *error);
extern bool doltgres_index_listed(HostIoIo *io, const PgRel *rel, bool *has_result, HostOwnedHandle *result,
								  HostError *error);
extern bool doltgres_index_removal(const PgCatalogCaps *caps, HostIoIo *io, const PgRel *rel,
								   const access_TupleUsizeOrderKeyType_t *table_key, size_t table_key_len,
								   const TxnBaseKey *recorded, HostOwnedHandle *result, PgPgError *error);
extern bool doltgres_index_opened(const PgCatalogCaps *caps, HostIoIo *io, HostOwnedHandle handle,
								  DoltgresIndexWork work, void *captures, PgPgError *error);
extern bool doltgres_index_removed(const PgCatalogCaps *caps, HostIoIo *io, const PgRel *rel, HostOwnedHandle handle,
								   const TxnMemtableVersionWriter *writer, DoltgresIndexRemoving *rows,
								   DoltgresIndexRemove remove, void *remove_captures, bool *result,
								   PgPgError *error);
#endif

#endif							/* DOLTGRES_DOLTGRES_INDEX_H */
