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
 * The server's host (pghost's lib.rs, design item 14): host's seams over
 * Postgres, through pg's wrappers, and the doors the server's entries hand
 * the storage crates.
 */
#ifndef DOLTGRES_PGHOST_LIB_H
#define DOLTGRES_PGHOST_LIB_H

#ifndef FRONTEND

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/pg/lib.h"
#include "doltgres/shmem/cache.h"
#include "doltgres/std/c_str.h"

/*
 * PghostReform is the server's re-form of a fold's row into its table's
 * layout in the catalog, which the reformed door calls with the catalog
 * capability the Io carries.
 */
typedef bool (*PghostReform) (const PgCatalogCaps *caps, const HostReformRow *row, HostReformedRow *result,
							  PgPgError *error);

extern HostIoWaits pghost_waits(void);
extern HostIoIo pghost_io(const PgMayRaise *raise);
extern HostIoIo pghost_io_catalog(const PgCatalogCaps *caps);
extern HostSnapshot pghost_any(void);
extern bool pghost_snapshot(const PgMayRaise *raise, const PgSnapshot *snapshot, HostSnapshot *result,
							PgPgError *error);
extern const PgMayRaise *pghost_pg_raise(const HostMayRaise *raise);
extern ShmemCacheLayout pghost_cache_layout(size_t bytes);
extern PgPgError pghost_raised(HostError *error);
extern HostError pghost_held(PgPgError *error);
extern void pghost_install_reform(PghostReform reform);
extern void pghost_install(void);

#ifdef DOLTGRES_TEST_PGHOST
extern void test_pghost_tests_timestamps_convert_from_postgres_epoch(void);
#endif

#endif							/* FRONTEND */

#endif							/* DOLTGRES_PGHOST_LIB_H */
