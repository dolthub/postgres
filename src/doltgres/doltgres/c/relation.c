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
 * Whether the fork stores a relation (doltgres' relation.rs): the one
 * function every entry that asks it calls, over the relation's relcache
 * entry and the backend's record of the relation it is creating.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/access/relation.h"
#include "doltgres/doltgres/relation.h"
#include "doltgres/host/local.h"
#include "pg_calls.h"
#include "doltgres/std/atomic.h"
#ifdef DOLTGRES_TEST_DOLTGRES
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "relation.rs"

/*
 * DoltgresRelationCreating is Creating: the relation a backend is creating,
 * the relation it is made to rewrite, and the transaction level that creates
 * it.
 */
typedef struct DoltgresRelationCreating
{
	StdAtomicU32 relation;
	StdAtomicU32 rewrites;
	StdAtomicU64 level;
} DoltgresRelationCreating;

/* doltgres_relation_Creating_Default_default is Creating's derived Default: no creation. */
static void
doltgres_relation_Creating_Default_default(void *result, const StdLocation *caller)
{
	DoltgresRelationCreating *self = result;

	std_AtomicU32_new(&self->relation, 0);
	std_AtomicU32_new(&self->rewrites, 0);
	std_AtomicU64_new(&self->level, 0);
}

/* DOLTGRES_RELATION_CREATING_TYPE is Creating's size, Default, and drop, which frees nothing. */
static const StdDefaultType DOLTGRES_RELATION_CREATING_TYPE = {sizeof(DoltgresRelationCreating),
_Alignof(DoltgresRelationCreating), doltgres_relation_Creating_Default_default, NULL};

/* doltgres_relation_Creating_set records a creation, in place of the one before it. */
static void
doltgres_relation_Creating_set(DoltgresRelationCreating *self, uint32 relation, uint32 rewrites, uint64 level)
{
	std_AtomicU32_store_relaxed(&self->relation, relation);
	std_AtomicU32_store_relaxed(&self->rewrites, rewrites);
	std_AtomicU64_store_relaxed(&self->level, level);
}

/*
 * doltgres_relation_Creating_rewrites returns what a relation is made to
 * rewrite when it is the one being created at this transaction level, and
 * zero for any other relation or level.
 */
static uint32
doltgres_relation_Creating_rewrites(DoltgresRelationCreating *self, uint32 relation, uint64 level)
{
	bool		named = std_AtomicU32_load_relaxed(&self->relation) == relation &&
		std_AtomicU64_load_relaxed(&self->level) == level;

	return named ? std_AtomicU32_load_relaxed(&self->rewrites) : 0;
}

/*
 * DOLTGRES_RELATION_CREATING is CREATING: the backend's newest creation, its
 * BackendLocal header and the process's value.
 */
static struct
{
	HostLocalBackendLocal local;
	DoltgresRelationCreating process;
}			DOLTGRES_RELATION_CREATING = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/*
 * doltgres_relation_relation_creating records the relation Postgres is about
 * to create and the relation it is made to rewrite, zero for none, as the
 * creation's call site hands them. The record is the transaction level's, so
 * it speaks for nothing once that transaction or subtransaction ends.
 */
void
doltgres_relation_relation_creating(uint32 relation, uint32 rewrites)
{
	HostLocalLocal local = host_local_BackendLocal_get(&DOLTGRES_RELATION_CREATING.local,
													   &DOLTGRES_RELATION_CREATING_TYPE);
	DoltgresRelationCreating *creating = (DoltgresRelationCreating *)
		host_local_Local_Deref_deref(&local, &DOLTGRES_RELATION_CREATING_TYPE);

	doltgres_relation_Creating_set(creating, relation, rewrites, pg_calls_transaction_level());
	host_local_Local_drop(&local, &DOLTGRES_RELATION_CREATING_TYPE);
}

/*
 * doltgres_relation_rewrites returns what a relation was made to rewrite:
 * its pg_class row's, or the creation's record while the row's is unset.
 */
uint32
doltgres_relation_rewrites(uint32 relation, uint32 row)
{
	HostLocalLocal local;
	uint32		rewrites;

	if (row != 0)
		return row;
	local = host_local_BackendLocal_get(&DOLTGRES_RELATION_CREATING.local, &DOLTGRES_RELATION_CREATING_TYPE);
	rewrites = doltgres_relation_Creating_rewrites((DoltgresRelationCreating *)
												   host_local_Local_Deref_deref(&local,
																				&DOLTGRES_RELATION_CREATING_TYPE),
												   relation, pg_calls_transaction_level());
	host_local_Local_drop(&local, &DOLTGRES_RELATION_CREATING_TYPE);
	return rewrites;
}

/*
 * doltgres_relation_twin returns whether a relation is a heap twin: its
 * access method's handler is the test module's dg_twin_handler. Only a build
 * with injection points has twins.
 */
static bool
doltgres_relation_twin(const PgCatalogCaps *caps, const PgRel *rel, bool *result, PgPgError *error)
{
#ifdef USE_INJECTION_POINTS
	return pg_calls_am_twin(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), pg_calls_rel_am(rel),
							result, error);
#else
	*result = false;
	return true;
#endif
}

/*
 * doltgres_relation_stored returns whether the fork stores a relation, by
 * access's rule; what the relation was made to rewrite is its relcache
 * entry's, or the creation's record while the entry's is unset. A heap twin
 * is never the fork's.
 */
bool
doltgres_relation_stored(const PgCatalogCaps *caps, const PgRel *rel, bool *result, PgPgError *error)
{
	uint32		relation = pg_calls_rel_id(rel);
	bool		stored = access_relation_stored(relation, pg_calls_rel_kind(rel),
												doltgres_relation_rewrites(relation, pg_calls_rel_rewrite(rel)));
	bool		twin;

	if (!doltgres_relation_twin(caps, rel, &twin, error))
		return false;
	*result = stored && !twin;
	return true;
}

/*
 * doltgres_relation_twin_table returns whether the table of an index is a
 * heap twin, by its pg_class row's access method; a build without injection
 * points has none.
 */
bool
doltgres_relation_twin_table(const PgMayRaise *raise, const PgCatalog *catalog, uint32 table, bool *result,
							 PgPgError *error)
{
#ifdef USE_INJECTION_POINTS
	uint32		am;

	return pg_calls_class_am(raise, catalog, table, &am, error) && pg_calls_am_twin(raise, catalog, am, result, error);
#else
	*result = false;
	return true;
#endif
}

#ifdef DOLTGRES_TEST_DOLTGRES

/*
 * test_doltgres_relation_tests_a_creation_speaks_for_its_relation_at_its_level
 * checks that a creation's record speaks for its relation at its transaction
 * level alone, and the next creation takes its place.
 */
void
test_doltgres_relation_tests_a_creation_speaks_for_its_relation_at_its_level(void)
{
	DoltgresRelationCreating creating;

	doltgres_relation_Creating_Default_default(&creating, NULL);
	STD_ASSERT(115, 9, doltgres_relation_Creating_rewrites(&creating, 20000, 1) == 0,
			   "creating.rewrites(20000, 1) == 0");
	doltgres_relation_Creating_set(&creating, 20000, 1259, 1);
	STD_ASSERT(117, 9, doltgres_relation_Creating_rewrites(&creating, 20000, 1) == 1259,
			   "creating.rewrites(20000, 1) == 1259");
	STD_ASSERT(118, 9, doltgres_relation_Creating_rewrites(&creating, 20001, 1) == 0 &&
			   doltgres_relation_Creating_rewrites(&creating, 20000, 2) == 0,
			   "creating.rewrites(20001, 1) == 0 && creating.rewrites(20000, 2) == 0");
	doltgres_relation_Creating_set(&creating, 20001, 0, 1);
	STD_ASSERT(120, 9, doltgres_relation_Creating_rewrites(&creating, 20000, 1) == 0 &&
			   doltgres_relation_Creating_rewrites(&creating, 20001, 1) == 0,
			   "creating.rewrites(20000, 1) == 0 && creating.rewrites(20001, 1) == 0");
}

#endif							/* DOLTGRES_TEST_DOLTGRES */

#endif							/* FRONTEND */
