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
 * An index's entries (txn's entry.rs): an entry's change, a unique index's
 * immediately checked change, and the records an index whose entries need
 * user code to form keeps beside them.
 */
#ifndef DOLTGRES_TXN_ENTRY_H
#define DOLTGRES_TXN_ENTRY_H

#include "doltgres/chunks/trailer.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/insert.h"
#include "doltgres/txn/memtable.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/status.h"

/*
 * TxnEntryEntry is Entry: a change of an index's entry, its writer and the
 * slot of its top-level entry in the commit status table, the index's
 * storage, its table's and its own object ids, the entry's key, what it
 * does, and whether the storage is logged.
 */
typedef struct TxnEntryEntry
{
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
	HostStorage storage;
	uint32		table;
	uint32		index;
	/* key is a Vec<u8>. */
	StdVec		key;
	TxnMemtableOp op;
	bool		logged;
} TxnEntryEntry;

/*
 * TXN_ENTRY_RECORD starts the key of an index's record of a row's entry,
 * which sorts after every entry, since every entry key starts with a
 * column's marker.
 */
#define TXN_ENTRY_RECORD ((uint8) 0xFF)

/* TxnEntryChangedTag is Changed's variants. */
typedef enum TxnEntryChangedTag
{
	/* The change pushed the entry. */
	TXN_ENTRY_CHANGED_DONE,
	/* The key holds an entry that exists to a Dirty read, with its value. */
	TXN_ENTRY_CHANGED_EXISTS,
	/* For a partial check, another transaction is writing the key. */
	TXN_ENTRY_CHANGED_RUNNING,
} TxnEntryChangedTag;

/* TxnEntryChanged is Changed: what a unique index's entry change did. */
typedef struct TxnEntryChanged
{
	TxnEntryChangedTag tag;
	union
	{
		/* exists is a Vec<u8>. */
		StdVec		exists;
	}			u;
} TxnEntryChanged;

extern void txn_entry_record(const uint8 *identity, size_t identity_len, StdVec *result);
extern bool txn_entry_change(HostIoIo *io, const HostSharedHeld *held, TxnEntryEntry *entry, bool has_record,
							 txn_TupleVecU8Op_t *record, HostError *error);
extern bool txn_entry_unique(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							 TxnEntryEntry *entry, bool has_recorded, txn_TupleVecU8Op_t *recorded,
							 const TxnInsertUniqueness *keyed, HostIoWaits *waits, HostRow row, bool partial,
							 TxnEntryChanged *result, HostError *error);
extern bool txn_entry_based(const uint8 *value, size_t value_len, const ChunksTrailerStamp *stamps,
							size_t stamps_len, bool keyless, StdBytes *result, HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_entry_tests_an_index_entrys_changes_are_logged_pushed_and_redone_once(void);
extern void test_txn_entry_tests_an_index_base_entry_reads_back_and_refuses_a_keyed_stamp_and_an_uneven_count(void);
extern void test_txn_entry_tests_an_index_storages_large_keys_spill_at_the_threshold(void);
extern void test_txn_entry_tests_a_unique_entry_takes_a_free_key_and_answers_an_existing_entrys_value(void);
extern void test_txn_entry_tests_a_counted_unique_entry_writes_nothing_for_its_own_copy_and_answers_another(void);
extern void test_txn_entry_tests_a_partial_unique_check_answers_a_running_write_without_waiting(void);
extern void test_txn_entry_tests_an_entry_and_its_record_change_together(void);
#endif

#endif							/* DOLTGRES_TXN_ENTRY_H */
