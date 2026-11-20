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
 * A fold's re-form of a keyed table's rows to the end of its pending
 * transforms (doltgres' reform.rs): the catalog's Reform a fold takes, and
 * the server's re-form of a row that host's reformed door calls.
 */
#ifndef DOLTGRES_DOLTGRES_REFORM_H
#define DOLTGRES_DOLTGRES_REFORM_H

#ifndef FRONTEND
#include "doltgres/access/key.h"
#include "doltgres/host/lib.h"
#include "doltgres/pg/lib.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/fold.h"

/*
 * DoltgresReformComposed is an entry of Catalog's composed map: a chain's
 * count of transforms and their encoded composition.
 */
typedef struct DoltgresReformComposed
{
	size_t		count;
	StdVec		bytes;
} DoltgresReformComposed;

/*
 * DoltgresReformCatalog is Catalog: a fold's re-form through the catalog's
 * layout of the table it folds, the table's OID, the bytes that say which
 * columns its key holds, and each chain's composed transform, encoded, by
 * how many transforms it applies, the map a Vec<Composed> sorted by count,
 * as the port's internals may hold it.
 */
typedef struct DoltgresReformCatalog
{
	uint32		relation;
	StdVec		keyed;
	StdVec		composed;
} DoltgresReformCatalog;

/* DOLTGRES_REFORM_CATALOG_REFORM_OPS is Catalog's Reform. */
extern const TxnFoldReformOps DOLTGRES_REFORM_CATALOG_REFORM_OPS;

extern void doltgres_reform_Catalog_new(const PgRel *rel, const access_TupleUsizeOrderKeyType_t *key, size_t key_len,
										DoltgresReformCatalog *result);
extern void doltgres_reform_Catalog_drop(DoltgresReformCatalog *self);
extern bool doltgres_reform_reformed_row(const PgCatalogCaps *caps, const HostReformRow *row, HostReformedRow *result,
										 PgPgError *error);
#endif

#endif							/* DOLTGRES_DOLTGRES_REFORM_H */
