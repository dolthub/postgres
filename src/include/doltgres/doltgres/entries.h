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
 * The fork's entries (doltgres' entries.rs): the functions Postgres calls,
 * and the shared kinds its injection points' tests keep, whose forms the
 * crate's other files take before entries' port defines them.
 */
#ifndef DOLTGRES_DOLTGRES_ENTRIES_H
#define DOLTGRES_DOLTGRES_ENTRIES_H

#include "doltgres/host/shared.h"

#ifndef FRONTEND
#include "doltgres/pg/lib.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/status.h"

extern bool doltgres_entries_blob_bytes(HostIoIo *io, const uint8 address[32], StdVec *result, PgPgError *error);
extern bool doltgres_entries_current(HostIoIo *io, uint32 command, TxnMemtableVersionWriter *result_0,
									 TxnStatusRoomed *result_1, PgPgError *error);
#endif

#ifdef USE_INJECTION_POINTS
/* The injection points' tests' kinds of shared records, each its Shape and how the server keeps it. */
extern const HostSharedPoolOps DOLTGRES_ENTRIES_TESTED_POOL_OPS;
extern const HostSharedPoolOps DOLTGRES_ENTRIES_OTHER_POOL_OPS;
extern const HostSharedSingleOps DOLTGRES_ENTRIES_TABLE_SINGLE_OPS;
extern const HostSharedProcSlotsOps DOLTGRES_ENTRIES_WORDS_PROC_SLOTS_OPS;
extern const HostSharedProcSlotsOps DOLTGRES_ENTRIES_STAYS_PROC_SLOTS_OPS;
extern const HostSharedSingleOps DOLTGRES_ENTRIES_SPARE_SINGLE_OPS;
extern const HostSharedAgedOps DOLTGRES_ENTRIES_OUTER_AGED_OPS;
extern const HostSharedAgedOps DOLTGRES_ENTRIES_INNER_AGED_OPS;
#endif

#endif							/* DOLTGRES_DOLTGRES_ENTRIES_H */
