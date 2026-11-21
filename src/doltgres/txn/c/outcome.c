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
 * A delta version's writer's outcome: the one resolve that visibility and
 * every reader of a version's writer use, read with no fork lock held, since
 * it asks Postgres.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/txn/outcome.h"
#include "doltgres/txn/status.h"

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/testing.h"
#endif

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "outcome.rs"

/*
 * txn_outcome_stamped returns whether the transaction of a row stamp's xid is
 * in progress, committed, or aborted, as outcome answers a version's writer
 * with no hint to cache: an xid no transaction has with no door, the
 * bootstrap and frozen ones committed and the invalid one aborted, and any
 * other through seam 1's status.
 */
bool
txn_outcome_stamped(HostIoIo *io, uint64 xid, HostStatus *result, HostError *error)
{
	HostXid		asked = {xid};

	if (xid == 0)
		*result = HOST_STATUS_ABORTED;
	else if (xid < TXN_STATUS_FIRST_NORMAL)
		*result = HOST_STATUS_COMMITTED;
	else
		return host_status(HOST_IO_IO_IO_DOORS(io), asked, result, error);
	return true;
}

/*
 * txn_outcome_outcome returns whether a version's writer is in progress,
 * committed, or aborted: an xid no transaction has with no door, as Postgres'
 * TransactionLogFetch answers them; else the version's hint, when a reader
 * cached one; else seam 1's status, whose committed or aborted answer it
 * caches in the hint. A speculative insert its writer killed is aborted,
 * whatever its transaction does after.
 */
bool
txn_outcome_outcome(HostIoIo *io, TxnMemtableVersionVersion *version, HostStatus *result, HostError *error)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
	uint64		writer = txn_memtable_version_Record_writer(&record).xid;
	HostXid		asked = {writer};

	if (txn_memtable_version_Version_killed(version))
	{
		*result = HOST_STATUS_ABORTED;
		return true;
	}
	if (writer < TXN_STATUS_FIRST_NORMAL)
		return txn_outcome_stamped(io, writer, result, error);
	switch (txn_memtable_version_Version_hint(version))
	{
		case TXN_MEMTABLE_VERSION_HINT_COMMITTED:
			*result = HOST_STATUS_COMMITTED;
			return true;
		case TXN_MEMTABLE_VERSION_HINT_ABORTED:
			*result = HOST_STATUS_ABORTED;
			return true;
		case TXN_MEMTABLE_VERSION_HINT_UNKNOWN:
			break;
	}
	if (!host_status(HOST_IO_IO_IO_DOORS(io), asked, result, error))
		return false;
	if (*result == HOST_STATUS_COMMITTED)
		txn_memtable_version_Version_set_hint(version, TXN_MEMTABLE_VERSION_HINT_COMMITTED);
	else if (*result == HOST_STATUS_ABORTED)
		txn_memtable_version_Version_set_hint(version, TXN_MEMTABLE_VERSION_HINT_ABORTED);
	return true;
}

#ifdef DOLTGRES_TEST_TXN

/* txn_outcome_tests_written pushes a version of a key into a storage's rows, written by a writer. */
static void
txn_outcome_tests_written(HostIoIo *io, HostSharedRecord state, const char *key, uint64 xid)
{
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "row", 3);
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xid), .op = &op,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	TxnMemtableVersionHandle handle;
	HostError	error;

	op.u.put.tuple.f0 = 1;
	op.u.put.heap_bytes = 32;
	if (!txn_memtable_table_push(io, state, (const uint8 *) key, strlen(key), &change, &handle, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_Op_drop(&op);
}

/* txn_outcome_tests_outcome returns a version's outcome. */
static HostStatus
txn_outcome_tests_outcome(HostIoIo *io, TxnMemtableVersionVersion *version)
{
	HostStatus	status;
	HostError	error;

	if (!txn_outcome_outcome(io, version, &status, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return status;
}

/*
 * test_txn_outcome_tests_a_writers_outcome_answers_frozen_with_no_door_and_caches_an_ended_writers:
 * a version FrozenTransactionId wrote reads committed with no door asked, so
 * its hint stays unset; a running writer's version reads in progress and
 * caches nothing; once the writer commits it reads committed and caches it,
 * and reads committed from the cache after the commit log keeps no such xid.
 */
void
test_txn_outcome_tests_a_writers_outcome_answers_frozen_with_no_door_and_caches_an_ended_writers(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9330);
	HostSharedRecord record = host_shared_Held_record(&held);
	HostXid		running = simc_begin();
	HostXid		later;
	HostDeltaHold hold;
	StdVec		frozen;
	StdVec		ran;
	TxnMemtableVersionVersion *version;
	HostError	error;

	txn_outcome_tests_written(io, record, "f", 2);
	txn_outcome_tests_written(io, record, "r", running.f0);
	if (!host_delta_hold(io, &hold, &error) ||
		!txn_memtable_table_versions(io, &hold, record, (const uint8 *) "f", 1, &frozen, &error) ||
		!txn_memtable_table_versions(io, &hold, record, (const uint8 *) "r", 1, &ran, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	version = &STD_VEC_AT(&frozen, TxnMemtableVersionVersion, 0);
	STD_ASSERT(__LINE__, 9, txn_outcome_tests_outcome(io, version) == HOST_STATUS_COMMITTED &&
			   txn_memtable_version_Version_hint(version) == TXN_MEMTABLE_VERSION_HINT_UNKNOWN,
			   "a frozen writer's version reads committed and caches nothing");
	version = &STD_VEC_AT(&ran, TxnMemtableVersionVersion, 0);
	STD_ASSERT(__LINE__, 9, txn_outcome_tests_outcome(io, version) == HOST_STATUS_IN_PROGRESS &&
			   txn_memtable_version_Version_hint(version) == TXN_MEMTABLE_VERSION_HINT_UNKNOWN,
			   "a running writer's version reads in progress and caches nothing");
	simc_commit(running);
	STD_ASSERT(__LINE__, 9, txn_outcome_tests_outcome(io, version) == HOST_STATUS_COMMITTED &&
			   txn_memtable_version_Version_hint(version) == TXN_MEMTABLE_VERSION_HINT_COMMITTED,
			   "a committed writer's version reads committed and caches it");
	later = simc_begin();
	simc_truncate_commit_log(later);
	STD_ASSERT(__LINE__, 9, txn_outcome_tests_outcome(io, version) == HOST_STATUS_COMMITTED,
			   "a cached commit reads committed after the commit log keeps no such xid");
	simc_commit(later);
	txn_memtable_version_versions_drop(&ran, 0);
	txn_memtable_version_versions_drop(&frozen, 0);
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_TXN */
