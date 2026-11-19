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
 * every kind of shared record the server keeps, listed once, built at its
 * first use from the crates' shapes, since C can't read their extern
 * constants in a constant expression (the port text's section 5). The
 * Rust's compile-time checks, that the table lists every aged kind before
 * the aged kinds its records name and that COMMITS names the commits' slots,
 * are checked in debug builds where the table is built and by its tests.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/chunks/packs/dropped.h"
#include "doltgres/chunks/packs/removal.h"
#include "doltgres/chunks/packs/roots.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/packs/single.h"
#include "doltgres/chunks/packs/writers.h"
#include "doltgres/doltgres/entries.h"
#include "doltgres/doltgres/kinds.h"
#include "doltgres/std/sync.h"
#include "doltgres/store/lib.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/memtable/pins.h"
#include "doltgres/txn/memtable/range.h"
#include "doltgres/txn/memtable/tuples.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/status.h"
#ifdef DOLTGRES_TEST_DOLTGRES
#include "doltgres/std/fmt.h"
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "kinds.rs"

/*
 * DOLTGRES_KINDS_MOST is the table's count of kinds, the injection points'
 * test kinds among them in a build with them.
 */
#ifdef USE_INJECTION_POINTS
#define DOLTGRES_KINDS_MOST 25
#else
#define DOLTGRES_KINDS_MOST 17
#endif

/* DOLTGRES_KINDS_TABLE is KINDS's lines, which its first use writes. */
static HostSharedKind DOLTGRES_KINDS_TABLE[DOLTGRES_KINDS_MOST];

/* DOLTGRES_KINDS_BUILT is the Once that writes the table once in the process. */
static StdOnce DOLTGRES_KINDS_BUILT = {STD_ONCE_INCOMPLETE};

/* DOLTGRES_KINDS_COMMITS is COMMITS: the commits' slots, the table's tenth kind. */
const HostSharedListed DOLTGRES_KINDS_COMMITS = {9};

/* doltgres_kinds_listed reports whether the table's kind at COMMITS's position is the commits' slots. */
static bool
doltgres_kinds_listed(const HostSharedKind *kinds)
{
	const HostSharedKind *kind = &kinds[DOLTGRES_KINDS_COMMITS.index];

	return kind->id == TXN_STATUS_COMMITS_PROC_SLOTS_OPS.id && kind->form.tag == HOST_SHARED_FORM_PROCESS &&
		kind->form.u.process.kept == TXN_STATUS_COMMITS_PROC_SLOTS_OPS.kept;
}

/* doltgres_kinds_build writes the table's lines, in KINDS's order. */
static void
doltgres_kinds_build(void *captures)
{
	HostSharedKind *kinds = DOLTGRES_KINDS_TABLE;
	size_t		n = 0;

	kinds[n++] = host_shared_Kind_pool(&CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS);
	kinds[n++] = host_shared_Kind_single(&CHUNKS_PACKS_SINGLE_CHUNKS_STATE_SINGLE_OPS);
	kinds[n++] = host_shared_Kind_slots(&CHUNKS_PACKS_WRITERS_WRITERS_PROC_SLOTS_OPS);
	kinds[n++] = host_shared_Kind_slots(&CHUNKS_PACKS_ROOTS_ROOTS_PROC_SLOTS_OPS);
	kinds[n++] = host_shared_Kind_slots(&CHUNKS_PACKS_REMOVAL_RESERVING_PROC_SLOTS_OPS);
	kinds[n++] = host_shared_Kind_pool(&CHUNKS_PACKS_DROPPED_DROPPED_REPOSITORY_POOL_OPS);
	kinds[n++] = host_shared_Kind_pool(&TXN_STATE_STATE_POOL_OPS);
	kinds[n++] = host_shared_Kind_single(&TXN_STATE_MAKERS_SINGLE_OPS);
	kinds[n++] = host_shared_Kind_single(&TXN_STATUS_TABLE_SINGLE_OPS);
	kinds[n++] = host_shared_Kind_slots(&TXN_STATUS_COMMITS_PROC_SLOTS_OPS);
	kinds[n++] = host_shared_Kind_single(&STORE_STORE_STATE_SINGLE_OPS);
	kinds[n++] = host_shared_Kind_pool(&TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS);
	kinds[n++] = host_shared_Kind_slots(&TXN_MEMTABLE_PINS_PINS_PROC_SLOTS_OPS);
	kinds[n++] = host_shared_Kind_aged(&TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS);
	kinds[n++] = host_shared_Kind_aged(&TXN_MEMTABLE_RANGE_RANGE_AGED_OPS);
	kinds[n++] = host_shared_Kind_aged(&TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS);
	kinds[n++] = host_shared_Kind_slots(&TXN_SPILL_PENDING_PROC_SLOTS_OPS);
#ifdef USE_INJECTION_POINTS
	kinds[n++] = host_shared_Kind_pool(&DOLTGRES_ENTRIES_TESTED_POOL_OPS);
	kinds[n++] = host_shared_Kind_pool(&DOLTGRES_ENTRIES_OTHER_POOL_OPS);
	kinds[n++] = host_shared_Kind_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS);
	kinds[n++] = host_shared_Kind_slots(&DOLTGRES_ENTRIES_WORDS_PROC_SLOTS_OPS);
	kinds[n++] = host_shared_Kind_slots(&DOLTGRES_ENTRIES_STAYS_PROC_SLOTS_OPS);
	kinds[n++] = host_shared_Kind_single(&DOLTGRES_ENTRIES_SPARE_SINGLE_OPS);
	kinds[n++] = host_shared_Kind_aged(&DOLTGRES_ENTRIES_OUTER_AGED_OPS);
	kinds[n++] = host_shared_Kind_aged(&DOLTGRES_ENTRIES_INNER_AGED_OPS);
#endif
	Assert(n == DOLTGRES_KINDS_MOST);
	Assert(host_shared_ordered(kinds, n));
	Assert(doltgres_kinds_listed(kinds));
}

/*
 * doltgres_kinds_KINDS returns KINDS, the kinds of shared records the server
 * keeps, and its count of kinds; its first use in the process builds it.
 */
const HostSharedKind *
doltgres_kinds_KINDS(size_t *len)
{
	std_once_call_once(&DOLTGRES_KINDS_BUILT, doltgres_kinds_build, NULL, RUST_FILE, 21, 1);
	*len = DOLTGRES_KINDS_MOST;
	return DOLTGRES_KINDS_TABLE;
}

#ifdef DOLTGRES_TEST_DOLTGRES

/* test_doltgres_kinds_tests_the_kinds_ids_are_distinct checks that no two kinds of the table have one id. */
void
test_doltgres_kinds_tests_the_kinds_ids_are_distinct(void)
{
	size_t		len;
	const HostSharedKind *kinds = doltgres_kinds_KINDS(&len);

	for (size_t at = 0; at < len; at++)
		for (size_t before = 0; before < at; before++)
			if (kinds[before].id == kinds[at].id)
			{
				static const StdFmtPiece pieces[] = {STD_FMT_LIT("two kinds have the id "), STD_FMT_ARG(0)};
				StdFmtArg	args[] = {{&kinds[at].id, std_u64_Display_fmt}};
				StdFmtArguments message = {pieces, lengthof(pieces), args};

				std_panic_fmt(RUST_FILE, 71, 13, &message);
			}
}

/*
 * test_doltgres_kinds_tests_the_table_is_ordered checks the Rust's
 * compile-time assert that the table lists every aged kind before the aged
 * kinds its records name.
 */
void
test_doltgres_kinds_tests_the_table_is_ordered(void)
{
	size_t		len;
	const HostSharedKind *kinds = doltgres_kinds_KINDS(&len);

	STD_ASSERT(61, 15, host_shared_ordered(kinds, len), "shared::ordered(KINDS)");
}

/*
 * test_doltgres_kinds_tests_commits_is_listed checks the Rust's compile-time
 * assert of Listed::at that COMMITS's position in the table is the commits'
 * slots.
 */
void
test_doltgres_kinds_tests_commits_is_listed(void)
{
	size_t		len;
	const HostSharedKind *kinds = doltgres_kinds_KINDS(&len);

	STD_ASSERT_MSG(58, 66, doltgres_kinds_listed(kinds),
				   "the server's table of kinds lists these slots at another position");
}

#endif							/* DOLTGRES_TEST_DOLTGRES */

#endif							/* FRONTEND */
