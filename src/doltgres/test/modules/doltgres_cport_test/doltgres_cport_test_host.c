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
 * The C port's host unit tests (the port text's section 13), host's crate
 * built with DOLTGRES_TEST_HOST as cargo builds its unit tests, run in a
 * backend: one row per test, in the Rust's order, with its outcome. Each
 * test runs in its own internal subtransaction, rolled back when it fails,
 * so a failure leaves nothing of Postgres' for the next.
 */
#include "postgres.h"

#include "access/xact.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/resowner.h"
#include "utils/tuplestore.h"

#include "doltgres_cport_test.h"

#include "doltgres/std/alloc.h"
#include "doltgres/std/btree.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/hash.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/str.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/host/cache.h"
#include "doltgres/host/delta/blocks.h"
#include "doltgres/host/delta/directory.h"
#include "doltgres/host/delta/page.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/lock.h"
#include "doltgres/host/shared.h"
#include "doltgres/host/shared/word.h"

PG_MODULE_MAGIC;

/* CPORT_TESTS are host's tests, in the order the Rust harness runs them, sorted by name. */
static const CportTest CPORT_TESTS[] = {
	{"cache::tests::a_key_holds_its_location_its_whole_address_and_its_whole_locator",
	test_host_cache_tests_a_key_holds_its_location_its_whole_address_and_its_whole_locator},
	{"cport::std::btree::tests::a_failed_allocation_drops_the_held_key_the_rest_then_the_peeked_one",
	test_std_btree_tests_a_failed_allocation_drops_the_held_key_the_rest_then_the_peeked_one},
	{"cport::std::btree::tests::a_panicking_ne_drops_the_held_key_the_rest_then_the_peeked_one",
	test_std_btree_tests_a_panicking_ne_drops_the_held_key_the_rest_then_the_peeked_one},
	{"cport::std::btree::tests::a_panicking_sort_drops_the_array_once",
	test_std_btree_tests_a_panicking_sort_drops_the_array_once},
	{"cport::std::btree::tests::a_set_from_an_array_drops_its_duplicates_then_its_keys",
	test_std_btree_tests_a_set_from_an_array_drops_its_duplicates_then_its_keys},
	{"cport::std::btree::tests::an_over_aligned_key_stays_aligned",
	test_std_btree_tests_an_over_aligned_key_stays_aligned},
	{"cport::std::hash::tests::random_state_hash_one_is_rusts",
	test_std_hash_tests_random_state_hash_one_is_rusts},
	{"cport::std::sort::tests::a_wide_copy_element_takes_the_fallback_small_sort",
	test_std_sort_tests_a_wide_copy_element_takes_the_fallback_small_sort},
	{"cport::std::str::tests::a_lossy_conversion_whose_growth_fails_leaks_nothing",
	test_std_str_tests_a_lossy_conversion_whose_growth_fails_leaks_nothing},
	{"cport::std::sync::tests::an_arc_whose_allocation_fails_drops_its_value",
	test_std_sync_tests_an_arc_whose_allocation_fails_drops_its_value},
	{"cport::std::sync::tests::an_arcs_into_inner_takes_its_value_only_from_the_last_strong_arc",
	test_std_sync_tests_an_arcs_into_inner_takes_its_value_only_from_the_last_strong_arc},
	{"cport::std::sync::tests::an_over_aligned_arc_value_stays_aligned",
	test_std_sync_tests_an_over_aligned_arc_value_stays_aligned},
	{"cport::std::vec::tests::an_over_aligned_element_stays_aligned",
	test_std_vec_tests_an_over_aligned_element_stays_aligned},
	{"delta::blocks::tests::a_body_takes_the_smallest_class_that_holds_it",
	test_host_delta_blocks_tests_a_body_takes_the_smallest_class_that_holds_it},
	{"delta::directory::tests::a_size_fits_its_word",
	test_host_delta_directory_tests_a_size_fits_its_word},
	{"delta::directory::tests::a_words_index_is_below_its_pages_count",
	test_host_delta_directory_tests_a_words_index_is_below_its_pages_count},
	{"delta::page::tests::a_name_is_a_number_and_a_generation",
	test_host_delta_page_tests_a_name_is_a_number_and_a_generation},
	{"delta::page::tests::a_released_slot_moves_on_and_its_last_generation_is_spent",
	test_host_delta_page_tests_a_released_slot_moves_on_and_its_last_generation_is_spent},
	{"delta::page::tests::a_size_is_a_body_no_larger_than_a_block",
	test_host_delta_page_tests_a_size_is_a_body_no_larger_than_a_block},
	{"delta::page::tests::a_stamp_is_passed_by_a_later_oldest_epoch",
	test_host_delta_page_tests_a_stamp_is_passed_by_a_later_oldest_epoch},
	{"delta::page::tests::a_word_holds_a_state_and_a_generation",
	test_host_delta_page_tests_a_word_holds_a_state_and_a_generation},
	{"delta::page::tests::an_open_reaches_a_page_a_hold_may_reach",
	test_host_delta_page_tests_an_open_reaches_a_page_a_hold_may_reach},
	{"delta::page::tests::an_open_refuses_a_slot_remade_between_its_loads",
	test_host_delta_page_tests_an_open_refuses_a_slot_remade_between_its_loads},
	{"delta::page::tests::each_transition_takes_only_its_own_state",
	test_host_delta_page_tests_each_transition_takes_only_its_own_state},
	{"delta::page::tests::reads_end_at_the_mark_and_appends_at_the_capacity",
	test_host_delta_page_tests_reads_end_at_the_mark_and_appends_at_the_capacity},
	{"lock::tests::a_drop_under_an_unwind_through_its_lock_defers",
	test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers},
	{"lock::tests::a_guard_gives_the_value_and_lends_the_held",
	test_host_lock_tests_a_guard_gives_the_value_and_lends_the_held},
	{"lock::tests::a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take",
	test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take},
	{"lock::tests::a_second_take_of_a_held_mutex_is_refused",
	test_host_lock_tests_a_second_take_of_a_held_mutex_is_refused},
	{"lock::tests::a_write_under_a_read_is_refused",
	test_host_lock_tests_a_write_under_a_read_is_refused},
	{"shared::tests::a_table_lists_an_aged_kind_before_what_it_names",
	test_host_shared_tests_a_table_lists_an_aged_kind_before_what_it_names},
	{"shared::word::tests::a_dropped_owner_gives_its_record_up",
	test_host_shared_word_tests_a_dropped_owner_gives_its_record_up},
	{"shared::word::tests::a_freed_record_is_gone_for_its_location",
	test_host_shared_word_tests_a_freed_record_is_gone_for_its_location},
	{"shared::word::tests::a_location_names_a_slot_and_a_generation",
	test_host_shared_word_tests_a_location_names_a_slot_and_a_generation},
	{"shared::word::tests::a_reclaim_does_not_free_a_record_claimed_since",
	test_host_shared_word_tests_a_reclaim_does_not_free_a_record_claimed_since},
	{"shared::word::tests::a_reclaim_frees_only_a_record_not_opened_since",
	test_host_shared_word_tests_a_reclaim_frees_only_a_record_not_opened_since},
	{"shared::word::tests::a_reclaim_given_up_frees_a_condemned_record",
	test_host_shared_word_tests_a_reclaim_given_up_frees_a_condemned_record},
	{"shared::word::tests::a_record_is_live_until_its_slot_is_given_back",
	test_host_shared_word_tests_a_record_is_live_until_its_slot_is_given_back},
	{"shared::word::tests::a_slot_freed_at_its_last_generation_is_spent",
	test_host_shared_word_tests_a_slot_freed_at_its_last_generation_is_spent},
	{"shared::word::tests::a_sweep_lists_a_record_published_at_both_loads",
	test_host_shared_word_tests_a_sweep_lists_a_record_published_at_both_loads},
	{"shared::word::tests::the_last_close_frees_a_condemned_record",
	test_host_shared_word_tests_the_last_close_frees_a_condemned_record},
	{"shared::word::tests::the_most_holders_fit_their_field",
	test_host_shared_word_tests_the_most_holders_fit_their_field},
	{"tests::a_second_host_is_refused",
	test_host_tests_a_second_host_is_refused},
	{"tests::elapsed_saturates",
	test_host_tests_elapsed_saturates},
	{"tests::the_doors_answer_from_the_installed_host",
	test_host_tests_the_doors_answer_from_the_installed_host},
	{"tests::the_process_keeps_one_backend_local_value",
	test_host_tests_the_process_keeps_one_backend_local_value},
};


PG_FUNCTION_INFO_V1(doltgres_cport_test_host);

/* doltgres_cport_test_host runs host's tests and returns each one's name and outcome. */
Datum
doltgres_cport_test_host(PG_FUNCTION_ARGS)
{
	return doltgres_cport_test_run_all(fcinfo, CPORT_TESTS, lengthof(CPORT_TESTS));
}

PG_FUNCTION_INFO_V1(doltgres_cport_test_trap);

/* doltgres_cport_test_trap runs intrinsics::abort's form, which kills the backend with the trap. */
Datum
doltgres_cport_test_trap(PG_FUNCTION_ARGS)
{
	std_intrinsics_abort();
}

PG_FUNCTION_INFO_V1(doltgres_cport_test_panic_in_cleanup);

/*
 * doltgres_cport_test_panic_in_cleanup panics while holding a value whose
 * drop holds an inner value and panics: off Windows the second panic runs
 * the inner value's drop and then is panic_in_cleanup (the hook's report,
 * the line that the panic can't unwind, and process::abort); on Windows it
 * fails fast after the second hook, the inner drop never run.
 */
Datum
doltgres_cport_test_panic_in_cleanup(PG_FUNCTION_ARGS)
{
	StdUnwind	unwind;

	std_heap_make();
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const char first[] = "the first panic";

		std_panic("doltgres_cport_test.rs", 1, 1, first, sizeof(first) - 1);
	}
	else
	{
		StdUnwind	inner;

		STD_UNWIND_PUSH(&inner);
		if (sigsetjmp(inner.jump, 0) == 0)
		{
			static const char second[] = "a drop's panic while unwinding";

			std_panic("doltgres_cport_test.rs", 2, 1, second, sizeof(second) - 1);
		}
		elog(LOG, "the inner drop ran");
		std_unwind_resume(&inner);
	}
}

/* doltgres_cport_test_inner_panic is the body that panics inside a drop's catch_unwind. */
static void
doltgres_cport_test_inner_panic(void *captures)
{
	static const char text[] = "a panic a drop catches";

	std_panic("doltgres_cport_test.rs", 3, 1, text, sizeof(text) - 1);
}

/* doltgres_cport_test_catch_in_cleanup_body panics, and its drop catches a panic of its own and goes on. */
static void
doltgres_cport_test_catch_in_cleanup_body(void *captures)
{
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const char first[] = "the first panic";

		std_panic("doltgres_cport_test.rs", 4, 1, first, sizeof(first) - 1);
	}
	else
	{
		StdCaught	caught;

		if (!std_catch_unwind(doltgres_cport_test_inner_panic, NULL, &caught))
			std_Caught_drop(&caught);
		std_unwind_resume(&unwind);
	}
}

PG_FUNCTION_INFO_V1(doltgres_cport_test_catch_in_cleanup);

/*
 * doltgres_cport_test_catch_in_cleanup returns the message of the panic a
 * catch_unwind takes when a drop that unwinding runs catches a panic of its
 * own: the first panic's, on every platform.
 */
Datum
doltgres_cport_test_catch_in_cleanup(PG_FUNCTION_ARGS)
{
	StdCaught	caught;
	const char *text;
	size_t		len;

	std_heap_make();
	if (std_catch_unwind(doltgres_cport_test_catch_in_cleanup_body, NULL, &caught))
		PG_RETURN_NULL();
	if (!std_Caught_downcast_str(&caught, &text, &len))
		elog(ERROR, "the caught panic's payload is not a literal message");
	PG_RETURN_TEXT_P(cstring_to_text_with_len(text, len));
}

PG_FUNCTION_INFO_V1(doltgres_cport_test_alloc_failure);

/*
 * doltgres_cport_test_alloc_failure asks the global allocator for a block
 * past the port heap's largest, which host's tests' configuration serves with
 * System's allocator, as `cargo test -p host` does: handle_alloc_error runs
 * std's default alloc error hook, whose report the log shows, then
 * process::abort.
 */
Datum
doltgres_cport_test_alloc_failure(PG_FUNCTION_ARGS)
{
	StdUnwind	unwind;

	std_heap_make();
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		(void) std_alloc(SIZE_MAX / 2 + 1, 1);
		std_unwind_pop(&unwind);
	}
	else
		std_unwind_resume(&unwind);
	PG_RETURN_VOID();
}
