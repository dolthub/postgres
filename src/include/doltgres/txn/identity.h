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
 * A database's identity tree (txn's identity.rs): each schema's, relation's,
 * and column's pairs of its derived id and its Postgres number, their keys,
 * their lookups, their derivation, and their redo.
 */
#ifndef DOLTGRES_TXN_IDENTITY_H
#define DOLTGRES_TXN_IDENTITY_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/record.h"
#include "doltgres/txn/status.h"

typedef struct RepoRepo RepoRepo;

/* TXN_IDENTITY_OBJECT_ID_BIT is the bit every derived object id sets, which no Postgres number does. */
#define TXN_IDENTITY_OBJECT_ID_BIT (((uint64) 1) << 63)

/* TxnIdentityKind is Kind: the kind of an object the identity tree pairs, Doltgrust's catalog kind. */
typedef enum TxnIdentityKind
{
	TXN_IDENTITY_KIND_SCHEMA = 1,
	TXN_IDENTITY_KIND_RELATION = 2,
	TXN_IDENTITY_KIND_COLUMN = 3,
} TxnIdentityKind;

/* TxnIdentityInitial is one of INITIAL's schemas: its fixed OID and its name. */
typedef struct TxnIdentityInitial
{
	uint32		oid;
	const char *name;
	size_t		name_len;
} TxnIdentityInitial;

/* The OIDs of INITIAL's schemas, in its order, which doltgres checks against Postgres' own as it compiles. */
#define TXN_IDENTITY_INITIAL_0 11
#define TXN_IDENTITY_INITIAL_1 99
#define TXN_IDENTITY_INITIAL_2 2200

/*
 * TXN_IDENTITY_INITIAL is the schemas every database starts with, which
 * Postgres' bootstrap makes, by their fixed OIDs and their names:
 * pg_catalog, pg_toast, and public.
 */
extern const TxnIdentityInitial TXN_IDENTITY_INITIAL[3];

/*
 * TxnIdentityPair is Pair: a key and the value it gives the key, or none for
 * the key's removal.
 */
typedef struct TxnIdentityPair
{
	/* f0 is a Vec<u8>. */
	StdVec		f0;
	/* f1 is a Vec<u8>, or none. */
	bool		has_f1;
	StdVec		f1;
} TxnIdentityPair;

/*
 * TxnIdentityDatabase is Database: a database's identity tree as its readers
 * reach it, its repository, a reader's guard, and the repository's number.
 */
typedef struct TxnIdentityDatabase
{
	const RepoRepo *f0;
	const HostSharedReader *f1;
	uint32		f2;
} TxnIdentityDatabase;

/* TxnIdentityPaired is Paired: a key of the identity tree with the value it holds, two Vec<u8>s. */
typedef struct TxnIdentityPaired
{
	StdVec		f0;
	StdVec		f1;
} TxnIdentityPaired;

/* TxnIdentityKeyed returns the key of an id's pair by id. */
typedef void (*TxnIdentityKeyed) (const void *captures, uint64 id, StdVec *result);

extern bool txn_identity_derive_object_id(TxnIdentityKind kind, uint64 parent, const uint8 *name, size_t name_len,
										  uint64 *result, HostError *error);
extern bool txn_identity_initial(StdVec *result, HostError *error);
extern uint64 txn_identity_next_id(uint64 id);
extern void txn_identity_column_name(const uint8 *name, size_t name_len, uint8 category, StdVec *result);
extern void txn_identity_by_id(TxnIdentityKind kind, uint64 id, StdVec *result);
extern void txn_identity_by_oid(TxnIdentityKind kind, uint32 oid, StdVec *result);
extern void txn_identity_column_by_id(uint64 table, uint64 column, StdVec *result);
extern void txn_identity_column_by_number(uint64 table, uint16 attnum, StdVec *result);
extern void txn_identity_columns(uint64 table, bool ids, StdVec *result);
extern bool txn_identity_looked_up(HostIoIo *io, HostIoWaits *waits, TxnIdentityDatabase database, const uint8 *key,
								   size_t key_len, bool *has_result, StdVec *result, HostError *error);
extern bool txn_identity_id(const uint8 *value, size_t value_len, uint64 *result, HostError *error);
extern bool txn_identity_number(const uint8 *value, size_t value_len, uint32 *result, HostError *error);
extern bool txn_identity_derived(HostIoIo *io, HostIoWaits *waits, TxnIdentityDatabase database,
								 TxnMemtableVersionWriter writer, TxnStatusRoomed roomed, TxnIdentityKind kind,
								 uint64 parent, const uint8 *name, size_t name_len, const uint8 *numbered,
								 size_t numbered_len, uint32 number, TxnIdentityKeyed keyed,
								 const void *keyed_captures, void (*keyed_drop) (void *),
								 uint64 *result, HostError *error);
extern bool txn_identity_removed(HostIoIo *io, HostIoWaits *waits, uint32 repository, uint32 own,
								 TxnMemtableVersionWriter writer, TxnStatusRoomed roomed, StdVec *keys,
								 HostError *error);
extern bool txn_identity_folded(HostIoIo *io, uint32 repository, HostLsn point, StdVec *result, HostError *error);
extern bool txn_identity_dropped(HostIoIo *io, uint32 repository, HostLsn point, bool *took, HostError *error);
extern bool txn_identity_redo(HostIoIo *io, TxnRecordDatabaseChange *change, uint32 own, uint64 lsn,
							  HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_identity_tests_an_objects_id_derives_from_its_kind_parent_and_name(void);
extern void test_txn_identity_tests_a_columns_id_holds_its_types_category(void);
extern void test_txn_identity_tests_a_pairs_keys_find_it_from_either_side(void);
extern void test_txn_identity_tests_a_tables_pairs_read_back_by_its_oid_and_its_id(void);
extern void test_txn_identity_tests_an_aborted_transactions_pairs_go_at_the_take_back(void);
extern void test_txn_identity_tests_a_taken_id_moves_on_and_a_removed_pair_frees_it(void);
extern void test_txn_identity_tests_redone_identity_changes_give_the_same_pairs_once(void);
extern void test_txn_identity_tests_an_identity_change_reads_back_as_written(void);
extern void test_txn_identity_tests_a_saves_fold_keeps_its_committed_pairs_and_leaves_running_ones(void);
extern void test_txn_identity_tests_a_saves_drop_takes_its_folded_pairs_out(void);
extern void test_txn_identity_tests_an_identity_put_carries_no_locker(void);
extern void test_txn_identity_tests_a_spilled_removal_reads_above_the_pair_it_removes_whatever_the_listing(void);
extern void test_txn_identity_tests_a_transactions_spilled_removal_reads_above_its_spilled_pair(void);
#endif

#endif							/* DOLTGRES_TXN_IDENTITY_H */
