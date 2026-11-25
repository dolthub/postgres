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

#include "doltgres/std/hashmap.h"
#include "doltgres/txn/outcome.h"
#include "doltgres/txn/status.h"

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/testing.h"
#endif

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "outcome.rs"

/* TxnOutcomeMode is how the fold running in this process answers a writer: as no fold, while it records, or as decided. */
typedef enum TxnOutcomeMode
{
	TXN_OUTCOME_MODE_SHUT,
	TXN_OUTCOME_MODE_RECORDING,
	TXN_OUTCOME_MODE_DECIDED,
} TxnOutcomeMode;

/* TXN_OUTCOME_STATUS is the bits of a recording fold's answer that hold the writer's status. */
#define TXN_OUTCOME_STATUS 3

/* TXN_OUTCOME_KILLED_ABORTED marks a committed writer whose killed versions the fold answered aborted. */
#define TXN_OUTCOME_KILLED_ABORTED 4

/* txn_outcome_Answer_t is a recording fold's answer to an xid: its status and its killed versions' mark. */
typedef struct txn_outcome_Answer_t
{
	uint64		f0;
	uint8		f1;
} txn_outcome_Answer_t;

/* txn_outcome_xid_hash is an xid's Hash, as a recording fold's answers key it. */
static void
txn_outcome_xid_hash(const void *key, StdHasher state)
{
	std_Hasher_write_u64(state, *(const uint64 *) key);
}

/* txn_outcome_xid_eq is an xid's PartialEq, as a recording fold's answers compare keys. */
static bool
txn_outcome_xid_eq(const void *a, const void *b)
{
	return *(const uint64 *) a == *(const uint64 *) b;
}

/* TXN_OUTCOME_ANSWERS is the table of a recording fold's answers, a HashMap<u64, u8>. */
static const StdHashMapType TXN_OUTCOME_ANSWERS = {
	STD_HASHMAP_SHAPE(txn_outcome_Answer_t),
	.key_hash = txn_outcome_xid_hash,
	.key_eq = txn_outcome_xid_eq,
	.hasher = &FOLDHASH_FAST_RANDOM_STATE_BUILD_HASHER_OPS,
	.hasher_default = foldhash_fast_RandomState_Default_default,
};

/*
 * TxnOutcomeScope is the fold running in this process, which runs one fold
 * at a time: its mode, its point, its answer to each xid it asked while
 * recording, and
 * the writers it answered aborted and the committed writers whose killed
 * versions it answered aborted, each a Vec<u64>, ascending once decided.
 */
typedef struct TxnOutcomeScope
{
	TxnOutcomeMode mode;
	uint64		point;

	/*
	 * rolled_back is whether the scope answers a running top's
	 * subtransactions' memtable versions by their own status, as VACUUM's
	 * fold does, or leaves them in progress with their top.
	 */
	bool		rolled_back;
	StdHashMap	answers;
	StdVec		aborted;
	StdVec		killed;
} TxnOutcomeScope;

/* txn_outcome_scope is this process's fold's scope, shut while no fold runs. */
static TxnOutcomeScope txn_outcome_scope;

/* txn_outcome_held reports whether an ascending vector of xids holds one. */
static bool
txn_outcome_held(const StdVec *xids, uint64 xid)
{
	size_t		low = 0;
	size_t		high = xids->len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		uint64		at = STD_VEC_AT(xids, uint64, mid);

		if (at == xid)
			return true;
		if (at < xid)
			low = mid + 1;
		else
			high = mid;
	}
	return false;
}

/* txn_outcome_top returns a writer's top-level xid: its version's top when the version names one, else the xid itself. */
static uint64
txn_outcome_top(uint64 xid, TxnMemtableVersionVersion *version)
{
	TxnMemtableVersionRecord record;
	TxnMemtableVersionWriter writer;

	if (version == NULL)
		return xid;
	record = txn_memtable_version_Version_record(version);
	writer = txn_memtable_version_Record_writer(&record);
	return writer.has_top ? writer.top : writer.xid;
}

/*
 * txn_outcome_by_point answers a committed writer as every server folding the
 * scope's point answers it: committed when its top-level transaction's commit
 * record ends at or before the point, which every server holds the status
 * entry of, and in progress otherwise, since a standby folding the point may
 * not have replayed the commit (bugs row 432).
 */
static bool
txn_outcome_by_point(HostIoIo *io, uint64 top, HostStatus *result, HostError *error)
{
	HostXid		xid = {top};
	TxnStatusCommit commit;

	if (!txn_status_committed(io, xid, &commit, error))
		return false;
	*result = commit.tag == TXN_STATUS_COMMIT_AT && commit.u.at <= txn_outcome_scope.point ? HOST_STATUS_COMMITTED :
		HOST_STATUS_IN_PROGRESS;
	return true;
}

/*
 * txn_outcome_recorded answers a writer as a recording fold does, once per
 * xid: its first answer, from a version's hint or seam 1's status, stands
 * for the whole fold, a committed one answers as by_point does, and an
 * aborted one is logged. A killed version of a
 * committed writer is answered aborted and its writer logged.
 */
static bool
txn_outcome_recorded(HostIoIo *io, uint64 xid, TxnMemtableVersionVersion *version, bool killed,
					 HostStatus *result, HostError *error)
{
	TxnOutcomeScope *scope = &txn_outcome_scope;
	txn_outcome_Answer_t answer = {.f0 = xid};
	uint8	   *known = std_HashMap_get(&scope->answers, &TXN_OUTCOME_ANSWERS, &xid);
	HostStatus	status;
	uint8		old;

	if (known != NULL)
		answer.f1 = *(known + offsetof(txn_outcome_Answer_t, f1));
	else
	{
		HostXid		asked = {xid};
		TxnMemtableVersionHint hint = version == NULL ? TXN_MEMTABLE_VERSION_HINT_UNKNOWN :
			txn_memtable_version_Version_hint(version);

		if (hint == TXN_MEMTABLE_VERSION_HINT_COMMITTED)
			status = HOST_STATUS_COMMITTED;
		else if (hint == TXN_MEMTABLE_VERSION_HINT_ABORTED)
			status = HOST_STATUS_ABORTED;
		else if (!host_status(HOST_IO_IO_IO_DOORS(io), asked, &status, error))
			return false;
		if (status == HOST_STATUS_COMMITTED && !txn_outcome_by_point(io, txn_outcome_top(xid, version), &status, error))
			return false;
		if (status == HOST_STATUS_ABORTED)
			std_vec_push_Checked(&scope->aborted, &xid, sizeof(uint64), _Alignof(uint64));
		answer.f1 = (uint8) status;
	}
	status = (HostStatus) (answer.f1 & TXN_OUTCOME_STATUS);
	if (killed && status == HOST_STATUS_COMMITTED)
	{
		if ((answer.f1 & TXN_OUTCOME_KILLED_ABORTED) == 0)
		{
			std_vec_push_Checked(&scope->killed, &xid, sizeof(uint64), _Alignof(uint64));
			answer.f1 |= TXN_OUTCOME_KILLED_ABORTED;
		}
		status = HOST_STATUS_ABORTED;
	}
	(void) std_HashMap_insert(&scope->answers, &TXN_OUTCOME_ANSWERS, &answer.f0, &answer.f1, &old);
	*result = status;
	return true;
}

/*
 * txn_outcome_settle answers a writer as a decided fold does, the way its
 * record lets every server answer it alike: a writer it logged aborted is
 * aborted, a killed version is aborted only when its committed writer was
 * logged with it, a committed writer answers as by_point does, and any other stays in
 * progress, since the fold that recorded the point either read it in
 * progress, never read it, or read it committed past the point, which every
 * fold leaves alike.
 */
static bool
txn_outcome_settle(HostIoIo *io, uint64 xid, TxnMemtableVersionVersion *version, bool killed, HostStatus *result,
				   HostError *error)
{
	const TxnOutcomeScope *scope = &txn_outcome_scope;
	HostXid		asked = {xid};
	HostStatus	status;

	if (txn_outcome_held(&scope->aborted, xid))
		*result = HOST_STATUS_ABORTED;
	else if (killed)
		*result = txn_outcome_held(&scope->killed, xid) ? HOST_STATUS_ABORTED : HOST_STATUS_IN_PROGRESS;
	else if (version != NULL && txn_memtable_version_Version_hint(version) == TXN_MEMTABLE_VERSION_HINT_COMMITTED)
		return txn_outcome_by_point(io, txn_outcome_top(xid, version), result, error);
	else
	{
		if (!host_status(HOST_IO_IO_IO_DOORS(io), asked, &status, error))
			return false;
		if (status == HOST_STATUS_COMMITTED)
			return txn_outcome_by_point(io, txn_outcome_top(xid, version), result, error);
		*result = HOST_STATUS_IN_PROGRESS;
	}
	return true;
}

/* txn_outcome_scoped answers a writer in the running fold's scope: recorded while it records, settled once decided. */
static bool
txn_outcome_scoped(HostIoIo *io, uint64 xid, TxnMemtableVersionVersion *version, bool killed, HostStatus *result,
				   HostError *error)
{
	if (txn_outcome_scope.mode == TXN_OUTCOME_MODE_RECORDING)
		return txn_outcome_recorded(io, xid, version, killed, result, error);
	return txn_outcome_settle(io, xid, version, killed, result, error);
}

/*
 * txn_outcome_recording opens a recording fold's scope at its point: it
 * answers each writer once and logs those it answers aborted, reading a
 * running top's subtransactions' memtable versions only when rolled_back.
 */
void
txn_outcome_recording(uint64 point, bool rolled_back)
{
	TxnOutcomeScope *scope = &txn_outcome_scope;

	scope->mode = TXN_OUTCOME_MODE_RECORDING;
	scope->point = point;
	scope->rolled_back = rolled_back;
	std_HashMap_Default_default(&scope->answers, &TXN_OUTCOME_ANSWERS);
	std_vec_new(&scope->aborted, sizeof(uint64), _Alignof(uint64));
	std_vec_new(&scope->killed, sizeof(uint64), _Alignof(uint64));
}

/*
 * txn_outcome_decided opens a decided fold's scope at a recorded point, with
 * the writers its record logged aborted, or none; each list is cloned, since
 * the scope may outlive what holds the record.
 */
void
txn_outcome_decided(uint64 point, const StdVec *aborted, const StdVec *killed)
{
	TxnOutcomeScope *scope = &txn_outcome_scope;

	scope->mode = TXN_OUTCOME_MODE_DECIDED;
	scope->point = point;
	std_HashMap_Default_default(&scope->answers, &TXN_OUTCOME_ANSWERS);
	std_vec_new(&scope->aborted, sizeof(uint64), _Alignof(uint64));
	std_vec_new(&scope->killed, sizeof(uint64), _Alignof(uint64));
	if (aborted != NULL)
		std_vec_extend_from_slice_Checked(&scope->aborted, aborted->ptr, aborted->len, sizeof(uint64),
										  _Alignof(uint64));
	if (killed != NULL)
		std_vec_extend_from_slice_Checked(&scope->killed, killed->ptr, killed->len, sizeof(uint64),
										  _Alignof(uint64));
}

/* txn_outcome_aborting reports whether the running fold logged a writer aborted, so its install drops one. */
bool
txn_outcome_aborting(void)
{
	return txn_outcome_scope.aborted.len + txn_outcome_scope.killed.len > 0;
}

/* txn_outcome_order is u64's Ord, for qsort. */
static int
txn_outcome_order(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	return (x > y) - (x < y);
}

/*
 * txn_outcome_settled decides a recording fold as its record is logged: its
 * lists, sorted, are lent through aborted and killed for the record while
 * the scope stays open, and from then on it answers as every server decides
 * the record's point.
 */
void
txn_outcome_settled(StdVec *aborted, StdVec *killed)
{
	TxnOutcomeScope *scope = &txn_outcome_scope;

	qsort(scope->aborted.ptr, scope->aborted.len, sizeof(uint64), txn_outcome_order);
	qsort(scope->killed.ptr, scope->killed.len, sizeof(uint64), txn_outcome_order);
	scope->mode = TXN_OUTCOME_MODE_DECIDED;
	*aborted = scope->aborted;
	*killed = scope->killed;
}

/* txn_outcome_shut closes the running fold's scope, if any, freeing what it holds. */
void
txn_outcome_shut(void)
{
	TxnOutcomeScope *scope = &txn_outcome_scope;

	if (scope->mode == TXN_OUTCOME_MODE_SHUT)
		return;
	std_HashMap_drop(&scope->answers, &TXN_OUTCOME_ANSWERS);
	std_vec_free_Checked(&scope->aborted, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&scope->killed, sizeof(uint64), _Alignof(uint64));
	scope->mode = TXN_OUTCOME_MODE_SHUT;
}

/*
 * txn_outcome_stamped returns whether the transaction of a row stamp's xid is
 * in progress, committed, or aborted, as outcome answers a version's writer
 * with no hint to cache: an xid no transaction has with no door, the
 * bootstrap and frozen ones committed and the invalid one aborted, and any
 * other through seam 1's status, or the running fold's scope.
 */
bool
txn_outcome_stamped(HostIoIo *io, uint64 xid, HostStatus *result, HostError *error)
{
	HostXid		asked = {xid};

	if (xid == 0)
		*result = HOST_STATUS_ABORTED;
	else if (xid < TXN_STATUS_FIRST_NORMAL)
		*result = HOST_STATUS_COMMITTED;
	else if (txn_outcome_scope.mode != TXN_OUTCOME_MODE_SHUT)
		return txn_outcome_scoped(io, xid, NULL, false, result, error);
	else
		return host_status(HOST_IO_IO_IO_DOORS(io), asked, result, error);
	return true;
}

/*
 * txn_outcome_entry returns whether a chain entry's subtransaction is in
 * progress, committed, or aborted, by its own status: a decided fold's
 * record installed only with every entry committed or aborted, so there one
 * not committed is aborted.
 */
bool
txn_outcome_entry(HostIoIo *io, HostXid sub, HostStatus *result, HostError *error)
{
	if (!host_status(HOST_IO_IO_IO_DOORS(io), sub, result, error))
		return false;
	if (txn_outcome_scope.mode == TXN_OUTCOME_MODE_DECIDED && *result != HOST_STATUS_COMMITTED)
		*result = HOST_STATUS_ABORTED;
	return true;
}

/*
 * txn_outcome_outcome returns whether a version's writer is in progress,
 * committed, or aborted: an xid no transaction has with no door, as Postgres'
 * TransactionLogFetch answers them; else the version's hint, when a reader
 * cached one; else seam 1's status, whose committed or aborted answer it
 * caches in the hint. A speculative insert its writer killed is aborted,
 * whatever its transaction does after. Inside a fold's scope, a version
 * whose record ends past the fold's point is in progress, since no fold
 * reads it at that point; so is a subtransaction's version while the scope
 * answers its top-level transaction in progress, its own status unread,
 * since no fold can take a spilled one out of its top's spill before then
 * and a recording fold reads a memtable one only for VACUUM, whose fold the
 * spill's moves can't race (bugs row 432); any other writer is answered by
 * the scope.
 */
bool
txn_outcome_outcome(HostIoIo *io, TxnMemtableVersionVersion *version, HostStatus *result, HostError *error)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
	TxnMemtableVersionWriter named = txn_memtable_version_Record_writer(&record);
	uint64		writer = named.xid;
	HostXid		asked = {writer};
	uint64		lsn;

	if (txn_outcome_scope.mode != TXN_OUTCOME_MODE_SHUT && writer >= TXN_STATUS_FIRST_NORMAL)
	{
		if (txn_memtable_version_Record_lsn(&record, &lsn) && lsn > txn_outcome_scope.point)
		{
			*result = HOST_STATUS_IN_PROGRESS;
			return true;
		}
		if (named.has_top && named.top != writer &&
			(version->record.tag == TXN_MEMTABLE_VERSION_BODY_SPILLED ||
			 (txn_outcome_scope.mode == TXN_OUTCOME_MODE_RECORDING && !txn_outcome_scope.rolled_back)))
		{
			if (!txn_outcome_scoped(io, named.top, NULL, false, result, error))
				return false;
			if (*result == HOST_STATUS_IN_PROGRESS)
				return true;
		}
		return txn_outcome_scoped(io, writer, version, txn_memtable_version_Version_killed(version), result,
								  error);
	}
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
