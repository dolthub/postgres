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
