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
 * Which relations the fork stores (access' relation.rs, design item 9): the
 * rule, over a relation's plain values.
 */
#include "postgres.h"

#include "access/transam.h"
#include "catalog/pg_class_d.h"
#include "doltgres/access/relation.h"
#ifdef DOLTGRES_TEST_ACCESS
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "access" STD_SEP "src" STD_SEP "relation.rs"

/*
 * access_relation_stored reports whether the fork stores a relation of an
 * OID and a kind that was made to rewrite a relation, zero for none: its
 * OID is a user's, it is a table, a materialized view, or a TOAST table,
 * and the relation it was made to rewrite is a user's too.
 */
bool
access_relation_stored(uint32 relation, uint8 kind, uint32 rewrites)
{
	return relation >= FirstNormalObjectId &&
		(kind == RELKIND_RELATION || kind == RELKIND_MATVIEW || kind == RELKIND_TOASTVALUE) &&
		(rewrites == 0 || rewrites >= FirstNormalObjectId);
}

#ifdef DOLTGRES_TEST_ACCESS

/* ACCESS_RELATION_TESTS_USER is a user's relation's OID, and ACCESS_RELATION_TESTS_CATALOG a catalog's. */
#define ACCESS_RELATION_TESTS_USER (FirstNormalObjectId + 7)
#define ACCESS_RELATION_TESTS_CATALOG (FirstNormalObjectId - 1)

/*
 * The fork's own: of a user's relations of every kind Postgres has, the
 * tables, materialized views, and TOAST tables are the fork's, and nothing
 * initdb made is, whatever its kind.
 */
void
test_access_relation_tests_a_users_tables_materialized_views_and_toast_tables_are_stored(void)
{
	static const struct
	{
		uint8		kind;
		bool		is_stored;
	}			kinds[] = {
		{RELKIND_RELATION, true},
		{RELKIND_MATVIEW, true},
		{RELKIND_TOASTVALUE, true},
		{RELKIND_INDEX, false},
		{RELKIND_SEQUENCE, false},
		{RELKIND_VIEW, false},
		{RELKIND_COMPOSITE_TYPE, false},
		{RELKIND_FOREIGN_TABLE, false},
		{RELKIND_PARTITIONED_TABLE, false},
		{RELKIND_PARTITIONED_INDEX, false},
	};

	for (size_t i = 0; i < lengthof(kinds); i++)
	{
		STD_ASSERT(60, 13, access_relation_stored(ACCESS_RELATION_TESTS_USER, kinds[i].kind, 0) == kinds[i].is_stored,
				   "stored(USER, kind, 0) == is_stored");
		STD_ASSERT(61, 13, access_relation_stored(FirstNormalObjectId, kinds[i].kind, 0) == kinds[i].is_stored,
				   "stored(FirstNormalObjectId, kind, 0) == is_stored");
		STD_ASSERT(62, 13, !access_relation_stored(ACCESS_RELATION_TESTS_CATALOG, kinds[i].kind, 0),
				   "!stored(CATALOG, kind, 0)");
	}
}

/*
 * The fork's own: a relation made to rewrite a catalog keeps Postgres' code,
 * and one made to rewrite a user's relation is the fork's.
 */
void
test_access_relation_tests_a_relation_made_to_rewrite_a_catalog_is_not_stored(void)
{
	static const uint8 kinds[] = {RELKIND_RELATION, RELKIND_MATVIEW, RELKIND_TOASTVALUE};

	for (size_t i = 0; i < lengthof(kinds); i++)
	{
		STD_ASSERT(71, 13, !access_relation_stored(ACCESS_RELATION_TESTS_USER, kinds[i], ACCESS_RELATION_TESTS_CATALOG),
				   "!stored(USER, kind, CATALOG)");
		STD_ASSERT(72, 13, access_relation_stored(ACCESS_RELATION_TESTS_USER, kinds[i], FirstNormalObjectId),
				   "stored(USER, kind, FirstNormalObjectId)");
		STD_ASSERT(73, 13, !access_relation_stored(ACCESS_RELATION_TESTS_CATALOG, kinds[i], ACCESS_RELATION_TESTS_USER),
				   "!stored(CATALOG, kind, USER)");
	}
}
#endif
