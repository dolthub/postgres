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
 * Whether the fork stores a relation (doltgres' relation.rs), over the
 * relation's relcache entry and the backend's record of the relation it is
 * creating.
 */
#ifndef DOLTGRES_DOLTGRES_RELATION_H
#define DOLTGRES_DOLTGRES_RELATION_H

#ifndef FRONTEND

#include "doltgres/pg/lib.h"

extern void doltgres_relation_relation_creating(uint32 relation, uint32 rewrites);
extern uint32 doltgres_relation_rewrites(uint32 relation, uint32 row);
extern bool doltgres_relation_stored(const PgCatalogCaps *caps, const PgRel *rel, bool *result, PgPgError *error);
extern bool doltgres_relation_twin_table(const PgMayRaise *raise, const PgCatalog *catalog, uint32 table,
										 bool *result, PgPgError *error);

#ifdef DOLTGRES_TEST_DOLTGRES
extern void test_doltgres_relation_tests_a_creation_speaks_for_its_relation_at_its_level(void);
#endif

#endif							/* FRONTEND */

#endif							/* DOLTGRES_DOLTGRES_RELATION_H */
