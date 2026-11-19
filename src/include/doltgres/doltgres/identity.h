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
 * The identity pairs of the relations the fork stores (doltgres'
 * identity.rs): recorded at a relation's creation, removed at its drop, and
 * kept up by ALTER TABLE.
 */
#ifndef DOLTGRES_DOLTGRES_IDENTITY_H
#define DOLTGRES_DOLTGRES_IDENTITY_H

#ifndef FRONTEND

#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"
#include "doltgres/pg/lib.h"
#include "doltgres/repo/lib.h"
#include "nodes/parsenodes.h"

extern bool doltgres_identity_users(const PgRel *rel);
extern bool doltgres_identity_recorded(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *stored_0,
									   const HostSharedReader *stored_1, const PgRel *rel, uint32 command,
									   PgPgError *error);
extern bool doltgres_identity_toast_created(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *stored_0,
											const HostSharedReader *stored_1, const PgRel *rel, uint32 toast,
											PgPgError *error);
extern bool doltgres_identity_dropped(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *stored_0,
									  const HostSharedReader *stored_1, const PgRel *rel, PgPgError *error);
extern bool doltgres_identity_schema_created(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *stored_0,
											 const HostSharedReader *stored_1, uint32 namespace,
											 const uint8 *name, size_t name_len, PgPgError *error);
extern bool doltgres_identity_schema_dropped(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *stored_0,
											 const HostSharedReader *stored_1, uint32 namespace, PgPgError *error);
extern bool doltgres_identity_altered(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *stored_0,
									  const HostSharedReader *stored_1, const PgRel *rel, AlterTableType subtype,
									  PgPgError *error);

#endif							/* FRONTEND */

#endif							/* DOLTGRES_DOLTGRES_IDENTITY_H */
