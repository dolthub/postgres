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
 * The row layouts DDL changes (doltgres' layouts.rs): the transforms a
 * statement chains, the re-key of a keyless table's rows before its
 * transaction commits, and a rewrite's read of its table's old layout.
 */
#ifndef DOLTGRES_DOLTGRES_LAYOUTS_H
#define DOLTGRES_DOLTGRES_LAYOUTS_H

#ifndef FRONTEND
#include "doltgres/host/io.h"
#include "doltgres/host/owned.h"
#include "doltgres/pg/lib.h"
#include "nodes/parsenodes.h"

extern bool doltgres_layouts_altering(const PgCatalogCaps *caps, const PgRel *rel, AlterTableType subtype,
									  int32 rewrite, PgPgError *error);
extern bool doltgres_layouts_altered(const PgCatalogCaps *caps, const PgRel *rel, PgPgError *error);
extern bool doltgres_layouts_dropped(const PgCatalogCaps *caps, uint32 relation, int32 attnum, PgPgError *error);
extern bool doltgres_layouts_reading(const PgCatalogCaps *caps, HostIoIo *io, HostOwnedHandle scan,
									 const PgRel *relation, const PgSlot *slot, PgPgError *error);
extern bool doltgres_layouts_rekeyed(const PgCatalogCaps *caps, PgPgError *error);
extern void doltgres_layouts_sub_aborted(HostIoIo *io, uint32 subtransaction);
extern void doltgres_layouts_ended(HostIoIo *io);
#endif

#endif							/* DOLTGRES_DOLTGRES_LAYOUTS_H */
