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
 * The fork btree's scans (doltgres' entries/searches.rs), which the fork's
 * btree entries in entries.rs call.
 */
#ifndef DOLTGRES_DOLTGRES_ENTRIES_SEARCHES_H
#define DOLTGRES_DOLTGRES_ENTRIES_SEARCHES_H

#include "doltgres/access/nbtsearch.h"
#include "doltgres/host/owned.h"
#include "doltgres/pg/lib.h"

/* The backend's index scans, an owned kind whose records the resource owners' release ends. */
extern HostOwnedTable DOLTGRES_ENTRIES_SEARCHES_SEARCHES;

extern bool doltgres_entries_searches_begin(const PgCatalogCaps *caps, const PgRel *index, int32 nkeys,
											int32 norderbys, PgIndexScanDesc *result, PgPgError *error);
extern bool doltgres_entries_searches_rescan(const PgCaps *caps, PgIndexScan *scan, const PgScanKeys *keys,
											 PgPgError *error);
extern bool doltgres_entries_searches_end(const PgCaps *caps, PgIndexScan *scan, PgPgError *error);
extern bool doltgres_entries_searches_current(const PgCatalogCaps *caps, PgIndexScan *scan, PgTid *tid,
											  PgPgError *error);
extern bool doltgres_entries_searches_next(const PgCatalogCaps *caps, PgIndexScan *scan, int32 direction, bool *result,
										   PgPgError *error);
extern bool doltgres_entries_searches_bitmap(const PgCatalogCaps *caps, PgIndexScan *scan, PgBitmap *tbm,
											 int64 *result, PgPgError *error);
extern bool doltgres_entries_searches_mark(const PgCaps *caps, PgIndexScan *scan, PgPgError *error);
extern bool doltgres_entries_searches_restore(const PgCatalogCaps *caps, PgIndexScan *scan, PgPgError *error);

/* DOLTGRES_ENTRIES_SEARCHES_SCAN_CALLS_OPS is a scan's search calls; the doltgres_test extension's tests reach its step through it. */
extern const AccessNbtsearchSearchCallsOps DOLTGRES_ENTRIES_SEARCHES_SCAN_CALLS_OPS;

#endif							/* DOLTGRES_DOLTGRES_ENTRIES_SEARCHES_H */
