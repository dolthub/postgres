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
 * The fork's table scans (doltgres' scan.rs, design item 13, the access
 * map's E): each an owned record a scan's descriptor names by its handle,
 * reading a storage's base through trees' cursor, kept between the calls
 * that read its rows.
 */
#ifndef DOLTGRES_DOLTGRES_SCAN_H
#define DOLTGRES_DOLTGRES_SCAN_H

#include "doltgres/chunks/trailer.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/owned.h"
#include "doltgres/host/shared.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/lib.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/visible.h"

/* DOLTGRES_SCAN_INVALID_COMMAND is Postgres' InvalidCommandId, a row's deleter command when only locks are above it. */
#define DOLTGRES_SCAN_INVALID_COMMAND UINT32_MAX

/* DOLTGRES_SCAN_SPLITS_BUDGET is the bytes the fork's shared scan keeps for a parallel scan's split keys. */
#define DOLTGRES_SCAN_SPLITS_BUDGET 16384

/* DOLTGRES_SCAN_SPLITS_MOST is the most split keys a parallel scan stores. */
#define DOLTGRES_SCAN_SPLITS_MOST 64

/*
 * DoltgresScanStoresOps is Stores: what a scan's reads take their store
 * from under the scan's own reader, with lending a call the store that reads
 * under a reader, the backend's I/O passed through; the call keeps its
 * result in its captures.
 */
typedef struct DoltgresScanStoresOps
{
	void		(*with) (const void *self, HostIoIo *io, const HostSharedReader *reader,
						 void (*call) (void *captures, HostIoIo *io, TreesStoreNodeStore store), void *captures);
} DoltgresScanStoresOps;

/* DoltgresScanStores is a value that implements Stores. */
typedef struct DoltgresScanStores
{
	const DoltgresScanStoresOps *ops;
	const void *self;
} DoltgresScanStores;

/* DOLTGRES_SCAN_REPO_STORES_OPS is the backend's repository's Stores. */
extern const DoltgresScanStoresOps DOLTGRES_SCAN_REPO_STORES_OPS;

/*
 * DoltgresScanSerial is Serial: a serializable read's check of the row
 * versions it examines, its transaction's xmin and own xid, and the writers
 * the versions conflict out with, a Vec<Xid> its entry checks with Postgres
 * once the read is done.
 */
typedef struct DoltgresScanSerial
{
	TxnVisibleSerial read;
	StdVec	   *conflicts;
} DoltgresScanSerial;

/*
 * DoltgresScanUnder is Under: what a read needs of its storage beside the
 * store, the storage's state, the snapshot it reads under, and a
 * serializable read's check.
 */
typedef struct DoltgresScanUnder
{
	HostSharedRecord state;
	const HostSnapshot *snapshot;
	bool		has_serial;
	DoltgresScanSerial serial;
} DoltgresScanUnder;

/* doltgres_OptionVecU8_t is an Option<Vec<u8>>. */
typedef struct doltgres_OptionVecU8_t
{
	bool		has;
	StdVec		v;
} doltgres_OptionVecU8_t;

/*
 * DoltgresScanEpochs is Epochs: what gives a storage's rows the transforms
 * their reads take to the reader's epoch, its base's epoch, its live pending
 * transforms, the composed transform from each epoch below the reader's, the
 * pending transform the reader's epoch stops before, and whether the scan's
 * slot's layout was checked.
 */
typedef struct DoltgresScanEpochs
{
	uint32		base;
	/* chain is a Vec<Live>. */
	StdVec		chain;
	/* composed is a Vec<Option<Vec<u8>>>. */
	StdVec		composed;
	bool		has_end;
	size_t		end;
	bool		checked;
} DoltgresScanEpochs;

/* DoltgresScanDirection is Direction: the way a read moves through a scan's rows. */
typedef enum DoltgresScanDirection
{
	DOLTGRES_SCAN_DIRECTION_FORWARD,
	DOLTGRES_SCAN_DIRECTION_BACKWARD,
} DoltgresScanDirection;

/*
 * DoltgresScanChanger is Changer: what a row's stamp shows of the versions
 * above it, its changer's writer, its commander's raw command, and whether
 * the changer only locks the row.
 */
typedef struct DoltgresScanChanger
{
	uint64		xid;
	uint32		command;
	bool		locks;
} DoltgresScanChanger;

/*
 * DoltgresScanRow is Row: a row a read gives, its table's keyed layout and
 * object id, its entry's key, its tuple, its values' ids out of line, the
 * transform its read takes, its copy's row stamp, its writer's top-level
 * xid, its raw command, what its stamp shows above it, and what a Dirty read
 * found running.
 */
typedef struct DoltgresScanRow
{
	StdVec		keyed;
	uint32		table;
	StdVec		key;
	StdVec		tuple;
	StdVec		values;
	StdVec		transform;
	ChunksTrailerStamp stamp;
	uint64		top;
	uint32		command;
	bool		has_changer;
	DoltgresScanChanger changer;
	TxnVisibleDirty dirty;
} DoltgresScanRow;

/* DoltgresScanBitmapTag is Bitmap's variants. */
typedef enum DoltgresScanBitmapTag
{
	DOLTGRES_SCAN_BITMAP_TID,
	DOLTGRES_SCAN_BITMAP_LOSSY,
	DOLTGRES_SCAN_BITMAP_PAGE,
} DoltgresScanBitmapTag;

/*
 * DoltgresScanBitmap is Bitmap: a bitmap scan's next step, a tuple id of
 * its exact page to fetch, its lossy page's next row, or the bitmap's next
 * page.
 */
typedef struct DoltgresScanBitmap
{
	DoltgresScanBitmapTag tag;
	uint64		tid;
} DoltgresScanBitmap;

/* DoltgresScanLayout is Layout: what a fetch reads a table's rows by, its keyed layout, object id, and tree's order. */
typedef struct DoltgresScanLayout
{
	StdVec		keyed;
	uint32		table;
	TreesKeyOrder order;
} DoltgresScanLayout;

/*
 * DoltgresScanTrees is Trees: the trees of the base a read takes, its table
 * tree and its tuple id tree, each none for a storage with no base, its fold
 * point, and the storage's base generation the base was read at or after,
 * which a read checks after its delta read (txn_state_moved_past).
 */
typedef struct DoltgresScanTrees
{
	bool		has_root;
	TreesStoreTree root;
	bool		has_tuple_ids;
	TreesStoreTree tuple_ids;
	uint64		folded;
	uint64		generation;
} DoltgresScanTrees;

/*
 * DoltgresScanShared is Shared: the base a parallel scan's workers all read
 * as the leader stored it, its trees, fold point, fresh end, count of
 * entries, count of units, the delta's split keys (a Vec<Vec<u8>>), and its
 * epochs.
 */
typedef struct DoltgresScanShared
{
	bool		has_tree;
	TreesStoreTree tree;
	bool		has_tuple_ids;
	TreesStoreTree tuple_ids;
	uint64		folded;
	uint64		fresh_end;
	uint64		entries;
	uint64		units;
	StdVec		splits;
	DoltgresScanEpochs epochs;
} DoltgresScanShared;

/* DoltgresScanStepTag is Step's variants. */
typedef enum DoltgresScanStepTag
{
	DOLTGRES_SCAN_STEP_ROW,
	DOLTGRES_SCAN_STEP_UNIT,
} DoltgresScanStepTag;

/* DoltgresScanStep is Step: what a parallel scan's read gives, a row or the need of the next unit. */
typedef struct DoltgresScanStep
{
	DoltgresScanStepTag tag;
	DoltgresScanRow row;
} DoltgresScanStep;

/* DoltgresScanScan is Scan: a table scan's record, which SCANS holds. */
typedef struct DoltgresScanScan DoltgresScanScan;

/* DOLTGRES_SCAN_SCANS is SCANS, the backend's table scans. */
extern HostOwnedTable DOLTGRES_SCAN_SCANS;

/* doltgres_scan_Scan_OWNED_OPS is Scan's Owned: its kind, its table, and its release. */
extern const HostOwnedOwnedOps doltgres_scan_Scan_OWNED_OPS;

extern void doltgres_scan_Epochs_Default_default(DoltgresScanEpochs *result);
extern void doltgres_scan_Epochs_drop(DoltgresScanEpochs *self);
extern bool doltgres_scan_Epochs_of(HostIoIo *io, TreesStoreNodeStore store, uint32 epoch, bool has_chain,
									TreesStoreTree chain, DoltgresScanEpochs *result, HostError *error);
extern void doltgres_scan_Epochs_based(const TxnBaseBase *base, uint32 *result_0, bool *has_result_1,
									   TreesStoreTree *result_1);
extern bool doltgres_scan_Epochs_transform(DoltgresScanEpochs *self, uint32 epoch, StdVec *result, HostError *error);
extern bool doltgres_scan_Epochs_of_written(DoltgresScanEpochs *self, uint64 xid, uint32 command, bool has_end,
											uint64 end, StdVec *result, HostError *error);
extern uint32 doltgres_scan_Epochs_base(const DoltgresScanEpochs *self);
extern void doltgres_scan_Row_drop(DoltgresScanRow *self);
extern void doltgres_scan_Layout_drop(DoltgresScanLayout *self);
extern void doltgres_scan_Shared_drop(DoltgresScanShared *self);
extern void doltgres_scan_Step_drop(DoltgresScanStep *self);
extern bool doltgres_scan_sample_next(HostIoIo *io, HostOwnedHandle handle, uint32 *result, HostError *error);
extern bool doltgres_scan_sample_block(HostIoIo *io, HostOwnedHandle handle, bool has_block, uint32 block,
									   StdVec *rows, HostError *error);
extern bool doltgres_scan_sample_page(HostIoIo *io, HostOwnedHandle handle, bool *has_result, uint32 *result_0,
									  uint16 *result_1, HostError *error);
extern bool doltgres_scan_sample_take(HostIoIo *io, HostOwnedHandle handle, uint16 offset, bool *has_result,
									  DoltgresScanRow *result, HostError *error);
extern bool doltgres_scan_bitmap_page(HostIoIo *io, HostOwnedHandle handle, uint32 block, bool has_offsets,
									  const uint16 *offsets, size_t offsets_len, HostError *error);
extern bool doltgres_scan_bitmap_step(HostIoIo *io, HostOwnedHandle handle, DoltgresScanBitmap *result,
									  HostError *error);
extern bool doltgres_scan_bitmap_ended(HostIoIo *io, HostOwnedHandle handle, HostError *error);
extern bool doltgres_scan_blocks(HostIoIo *io, HostOwnedHandle handle, uint64 *result, HostError *error);
extern bool doltgres_scan_Direction_Debug_fmt(const void *value, struct StdFormatter *f);
extern DoltgresScanTrees doltgres_scan_Trees_of(const TxnBaseBase *base, uint64 generation);
extern bool doltgres_scan_scan_begin(HostIoIo *io, TreesStoreNodeStore store, uint64 blocks, const TxnBaseBase *base,
									 uint64 generation, StdVec *keyed, uint32 table, TreesKeyOrder *order,
									 HostOwnedHandle *result, HostError *error);
extern bool doltgres_scan_next(HostIoIo *io, const DoltgresScanStores *stores, const DoltgresScanUnder *under,
							   HostOwnedHandle handle, DoltgresScanDirection way, bool *has_result,
							   DoltgresScanRow *result, HostError *error);
extern bool doltgres_scan_Changer_Debug_fmt(const void *value, struct StdFormatter *f);
extern bool doltgres_scan_Changer_PartialEq_eq(const DoltgresScanChanger *self, const DoltgresScanChanger *other);
extern bool doltgres_scan_scan_rescan(HostIoIo *io, HostOwnedHandle handle, HostError *error);
extern bool doltgres_scan_seek(HostIoIo *io, const DoltgresScanStores *stores, HostOwnedHandle handle,
							   const uint8 *from, size_t from_len, DoltgresScanDirection way, HostError *error);
extern bool doltgres_scan_scan_end(HostIoIo *io, HostOwnedHandle handle, HostError *error);
extern bool doltgres_scan_tid_valid(HostIoIo *io, HostOwnedHandle handle, uint64 tuple_id, uint16 most_offset,
									bool *result, HostError *error);
extern bool doltgres_scan_fetch(HostIoIo *io, TreesStoreNodeStore store, const DoltgresScanUnder *under,
								DoltgresScanEpochs *epochs, DoltgresScanTrees trees, DoltgresScanLayout *layout,
								uint64 tuple_id, bool *has_result, DoltgresScanRow *result, HostError *error);
extern bool doltgres_scan_keyed(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint64 tuple_id,
								bool *has_result, StdVec *result_0, bool *result_1, HostError *error);
extern bool doltgres_scan_based(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint64 tuple_id,
								bool *result, HostError *error);
extern bool doltgres_scan_fetch_key(HostIoIo *io, TreesStoreNodeStore store, const DoltgresScanUnder *under,
									DoltgresScanEpochs *epochs, DoltgresScanTrees trees, DoltgresScanLayout *layout,
									const uint8 *key, size_t key_len, StdVec *result, HostError *error);
extern bool doltgres_scan_end_before(HostIoIo *io, HostOwnedHandle handle, uint64 xid, uint32 command, uint64 sub,
									 HostError *error);
extern bool doltgres_scan_unchecked(HostIoIo *io, HostOwnedHandle handle, bool *result, HostError *error);
extern bool doltgres_scan_set_range(HostIoIo *io, HostOwnedHandle handle, uint64 first, uint64 last,
									HostError *error);
extern bool doltgres_scan_range(HostIoIo *io, HostOwnedHandle handle, bool *has_result, uint64 *result_0,
								uint64 *result_1, HostError *error);
extern bool doltgres_scan_next_in_range(HostIoIo *io, TreesStoreNodeStore store, const DoltgresScanUnder *under,
										HostOwnedHandle handle, DoltgresScanDirection way, bool *has_result,
										DoltgresScanRow *result, HostError *error);
extern void doltgres_scan_splits(const StdVec *starts, size_t starts_len, StdVec *result);
extern bool doltgres_scan_split_keys(const uint8 *bytes, size_t bytes_len, StdVec *result, HostError *error);
extern bool doltgres_scan_begin_parallel(HostIoIo *io, DoltgresScanShared *shared, uint64 generation, StdVec *keyed,
										 uint32 table, TreesKeyOrder *order, HostOwnedHandle *result,
										 HostError *error);
extern bool doltgres_scan_is_parallel(HostIoIo *io, HostOwnedHandle handle, bool *result, HostError *error);
extern bool doltgres_scan_start_unit(HostIoIo *io, HostOwnedHandle handle, uint64 unit, HostError *error);
extern bool doltgres_scan_next_parallel(HostIoIo *io, const DoltgresScanStores *stores,
										const DoltgresScanUnder *under, HostOwnedHandle handle,
										DoltgresScanStep *result, HostError *error);
extern bool doltgres_scan_range_blocks(HostIoIo *io, HostOwnedHandle handle, uint64 blocks, uint32 *result_0,
									   uint32 *result_1, HostError *error);
extern bool doltgres_scan_start_range_unit(HostIoIo *io, HostOwnedHandle handle, uint64 block, HostError *error);

#ifdef DOLTGRES_TEST_DOLTGRES
extern void test_doltgres_scan_tests_a_scan_reads_a_bases_rows_in_order_across_calls(void);
extern void test_doltgres_scan_tests_a_scan_reads_backward_and_turns_as_heaps_does(void);
extern void test_doltgres_scan_tests_scans_of_one_base_keep_their_own_places(void);
extern void test_doltgres_scan_tests_a_keyless_entry_gives_each_copy_with_its_own_stamp(void);
extern void test_doltgres_scan_tests_a_fetch_gives_the_row_its_tuple_id_names(void);
extern void test_doltgres_scan_tests_each_keyless_copy_fetches_by_its_own_tuple_id(void);
extern void test_doltgres_scan_tests_a_tuple_id_is_valid_within_the_storages_blocks(void);
extern void test_doltgres_scan_tests_a_tid_range_read_gives_its_rows_in_tuple_id_order(void);
extern void test_doltgres_scan_tests_a_tid_range_pair_the_table_lacks_is_data_corruption(void);
extern void test_doltgres_scan_tests_a_parallel_scans_units_partition_the_entries(void);
extern void test_doltgres_scan_tests_a_parallel_scans_units_give_each_row_once(void);
extern void test_doltgres_scan_tests_a_parallel_tid_ranges_blocks_are_heaps(void);
extern void test_doltgres_scan_tests_a_scan_merges_the_deltas_rows_with_the_bases_in_key_order(void);
extern void test_doltgres_scan_tests_a_seek_reads_on_from_the_first_key_at_or_past_it_that_way(void);
extern void test_doltgres_scan_tests_a_keyed_tables_index_entry_is_one_copy_stamped_zero(void);
extern void test_doltgres_scan_tests_a_keyless_keys_rows_are_its_base_copies_the_delta_leaves_then_its_added_copies(void);
extern void test_doltgres_scan_tests_a_dirty_scan_gives_what_it_found_running_of_each_row(void);
extern void test_doltgres_scan_tests_a_fetch_reads_the_deltas_rows_and_the_bases_by_tuple_id(void);
extern void test_doltgres_scan_tests_a_compared_scan_merges_the_deltas_keys_in_the_tables_order(void);
extern void test_doltgres_scan_tests_a_tid_range_read_merges_the_deltas_tuple_ids_with_the_bases(void);
extern void test_doltgres_scan_tests_a_parallel_scans_units_merge_the_delta_each_key_once(void);
extern void test_doltgres_scan_tests_a_leaders_split_keys_are_its_range_starts_spread_within_the_budget(void);
extern void test_doltgres_scan_tests_a_parallel_scan_over_an_empty_base_reads_each_delta_key_once_by_the_leaders_splits(void);
extern void test_doltgres_scan_tests_a_sample_of_many_blocks_walks_the_delta_once(void);
extern void test_doltgres_scan_tests_a_scan_with_no_sample_or_range_leaves_its_answers_as_they_were(void);
extern void test_doltgres_scan_tests_a_scan_moves_to_a_newer_base_at_its_next_read_and_gives_every_row_once(void);
extern void test_doltgres_scan_tests_a_scan_held_inside_its_delta_read_across_an_install_and_drop_gives_every_row_once(void);
#endif

#endif							/* DOLTGRES_DOLTGRES_SCAN_H */
