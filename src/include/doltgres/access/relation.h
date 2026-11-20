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
 * Which relations the fork stores (access' relation.rs).
 */
#ifndef DOLTGRES_ACCESS_RELATION_H
#define DOLTGRES_ACCESS_RELATION_H

extern bool access_relation_stored(uint32 relation, uint8 kind, uint32 rewrites);

#ifdef DOLTGRES_TEST_ACCESS
extern void test_access_relation_tests_a_users_tables_materialized_views_and_toast_tables_are_stored(void);
extern void test_access_relation_tests_a_relation_made_to_rewrite_a_catalog_is_not_stored(void);
#endif

#endif							/* DOLTGRES_ACCESS_RELATION_H */
