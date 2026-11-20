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
 * The server's table of kinds (doltgres' kinds.rs, design item 14's S2):
 * every kind of shared record the server keeps, listed once.
 */
#ifndef DOLTGRES_DOLTGRES_KINDS_H
#define DOLTGRES_DOLTGRES_KINDS_H

#include "doltgres/host/shared.h"

/* DOLTGRES_KINDS_COMMITS is COMMITS: the commits' slots at their position in the table. */
extern const HostSharedListed DOLTGRES_KINDS_COMMITS;

extern const HostSharedKind *doltgres_kinds_KINDS(size_t *len);

#ifdef DOLTGRES_TEST_DOLTGRES
extern void test_doltgres_kinds_tests_the_kinds_ids_are_distinct(void);
extern void test_doltgres_kinds_tests_the_table_is_ordered(void);
extern void test_doltgres_kinds_tests_commits_is_listed(void);
#endif

#endif							/* DOLTGRES_DOLTGRES_KINDS_H */
