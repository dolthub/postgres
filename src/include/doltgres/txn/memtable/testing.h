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
 * What the memtable's tests share (txn's memtable/testing.rs): a range's
 * stand-in shapes on sim, a push that brings the pages it may need, and a
 * storage's state made in a repository of its own.
 */
#ifndef DOLTGRES_TXN_MEMTABLE_TESTING_H
#define DOLTGRES_TXN_MEMTABLE_TESTING_H

#include "doltgres/host/delta.h"
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable.h"
#include "doltgres/txn/memtable/index.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/state.h"

/* TXN_MEMTABLE_TESTING_KEYS_LOCK is KeysLock, the lock that owns a Keys record's pages. */
#define TXN_MEMTABLE_TESTING_KEYS_LOCK 0

/* TXN_MEMTABLE_TESTING_PAGE is PAGE, the size of a test's range page. */
#define TXN_MEMTABLE_TESTING_PAGE ((HostDeltaPageSize) {4096, 128})

extern const HostSharedPoolOps TXN_MEMTABLE_TESTING_HOLDER_POOL_OPS;
extern const HostSharedAgedOps TXN_MEMTABLE_TESTING_KEYS_AGED_OPS;
extern const TxnMemtableIndexKeyedOps TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS;

extern HostError txn_memtable_testing_failed(const char *what);
extern bool txn_memtable_testing_keys(HostIoIo *io, HostSharedHeld *result_0, uint64 *result_1, HostError *error);
extern TxnMemtableVersionWriter txn_memtable_testing_writer(uint64 xid);
extern TxnMemtableOp txn_memtable_testing_put(const uint8 *value, size_t value_len);
extern bool txn_memtable_testing_pushed(HostIoIo *io, uint64 keys, const uint8 *key, size_t key_len,
										const TxnMemtableOp *op, HostDeltaPageSize size,
										TxnMemtableVersionPushed *result, HostError *error);
extern void txn_memtable_testing_numbered(uint64 number, StdVec *result);
extern HostStorage txn_memtable_testing_storage(uint32 database);
extern bool txn_memtable_testing_stated(HostIoIo *io, uint32 database, HostSharedHeld *result, HostError *error);
extern bool txn_memtable_testing_held(HostIoIo *io, const HostStorage *storage, HostSharedHeld *result,
									  HostError *error);
extern HostSharedHeld txn_memtable_testing_created(HostIoIo *io, uint32 database);
extern bool txn_memtable_testing_ranged(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
										const StdBytes *firsts, size_t firsts_len, StdVec *result, HostError *error);

#endif							/* DOLTGRES_TXN_MEMTABLE_TESTING_H */
