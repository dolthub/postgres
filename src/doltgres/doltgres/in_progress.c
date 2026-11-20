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
 * The check of whether a transaction is in progress from any process (bugs
 * row 89): TransactionIdIsInProgress skips the proc array slot at the caller's
 * own offset, and an auxiliary process, which is never in the proc array,
 * keeps the zero offset of a slot another process holds, so this also reads
 * that slot under the proc array's lock.
 */

#include "postgres.h"

#include "access/subtrans.h"
#include "doltgres/in_progress.h"
#include "storage/proc.h"
#include "storage/procarray.h"

/*
 * doltgres_xid_in_progress says whether a transaction or a subtransaction is
 * in progress, as TransactionIdIsInProgress does, also for a caller outside the
 * proc array: the transaction in the slot at the caller's offset, its cached
 * subtransactions, and, past its cache, the ones pg_subtrans records.
 */
bool
doltgres_xid_in_progress(TransactionId xid)
{
	int			slot;
	TransactionId held;
	bool		found = false;
	bool		overflowed = false;

	if (TransactionIdIsInProgress(xid))
		return true;
	if (MyProc == NULL || !TransactionIdIsNormal(xid))
		return false;
	slot = MyProc->pgxactoff;
	LWLockAcquire(ProcArrayLock, LW_SHARED);
	held = ProcGlobal->xids[slot];
	if (TransactionIdEquals(held, xid))
		found = true;
	else if (TransactionIdIsValid(held) && TransactionIdPrecedes(held, xid))
	{
		for (uint32 at = 0; at < ProcGlobal->allProcCount; at++)
		{
			PGPROC	   *proc = &ProcGlobal->allProcs[at];

			if (proc == MyProc || proc->pgxactoff != slot || !TransactionIdEquals(proc->xid, held))
				continue;
			for (int sub = 0; sub < ProcGlobal->subxidStates[slot].count; sub++)
				found = found || TransactionIdEquals(proc->subxids.xids[sub], xid);
			overflowed = ProcGlobal->subxidStates[slot].overflowed;
		}
	}
	LWLockRelease(ProcArrayLock);
	if (!found && overflowed)
		found = TransactionIdEquals(SubTransGetTopmostTransaction(xid), held);
	return found;
}
