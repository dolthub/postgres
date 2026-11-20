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
 * The fork's test module's checks of answers of none (bugs row 288): each
 * calls a function of the server's C port that answers an Option through
 * out-params and reports whether an answer of none left its caller's value
 * as the caller had it. They include the C port's headers, which the rest of
 * the module keeps out.
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "access/relation.h"
#include "doltgres/access/nbtsearch.h"
#include "doltgres/doltgres/index.h"
#include "doltgres/pghost/lib.h"
#include "fmgr.h"
#include "utils/skipsupport.h"

/* DOLTGRES_ENTRIES_SEARCHES_SCAN_CALLS_OPS is a scan's search calls, whose step dg_step_unwritten calls. */
extern PGDLLIMPORT const AccessNbtsearchSearchCallsOps DOLTGRES_ENTRIES_SEARCHES_SCAN_CALLS_OPS;

PG_FUNCTION_INFO_V1(dg_listed_unwritten);
PG_FUNCTION_INFO_V1(dg_step_unwritten);

/*
 * dg_listed_unwritten answers whether a table whose removal of old entries
 * the backend doesn't hold, asked for by its relcache entry, answers none and
 * leaves its caller's handle as the caller had it (bugs row 288).
 */
Datum
dg_listed_unwritten(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	PgMayRaise	raise = {0};
	HostIoIo	io = pghost_io(&raise);
	HostOwnedHandle handle = {.slot = 0x5EED, .generation = 0x5EED};
	HostError	error;
	bool		listed = true;
	bool		ok = doltgres_index_listed(&io, (const PgRel *) rel, &listed, &handle, &error);

	relation_close(rel, AccessShareLock);
	if (!ok)
		elog(ERROR, "a table's removal of old entries couldn't be read");
	PG_RETURN_BOOL(!listed && handle.slot == 0x5EED && handle.generation == 0x5EED);
}

/* step_past_the_end is dg_step_unwritten's skip support: every value is its type's last. */
static Datum
step_past_the_end(Relation rel, Datum existing, bool *overflow)
{
	*overflow = true;
	return (Datum) 0;
}

/*
 * dg_step_unwritten answers whether a skip array's step past its type's last
 * value, increment and decrement alike, answers none and leaves its caller's
 * datum as the caller had it (bugs row 288). The step reads only its scan,
 * the first member of the search's calls.
 */
Datum
dg_step_unwritten(PG_FUNCTION_ARGS)
{
	SkipSupportData support = {.decrement = step_past_the_end, .increment = step_past_the_end};
	BTArrayKeyInfo array = {.sksup = &support};
	BTScanOpaqueData opaque = {.arrayKeys = &array, .arrayContext = CurrentMemoryContext};
	IndexScanDescData scan = {.opaque = &opaque};
	const IndexScanDescData *calls[2] = {&scan};
	PgCatalogCaps caps = {0};
	PgPgError	error;
	bool		unwritten = true;

	for (int k = 0; k < 2; k++)
	{
		uint64		datum = 0x5EED;
		bool		has = true;

		if (!DOLTGRES_ENTRIES_SEARCHES_SCAN_CALLS_OPS.calls.step(calls, &caps, 0, 7, k == 0, &has, &datum, &error))
			elog(ERROR, "a skip array's step failed");
		unwritten = unwritten && !has && datum == 0x5EED;
	}
	PG_RETURN_BOOL(unwritten);
}
