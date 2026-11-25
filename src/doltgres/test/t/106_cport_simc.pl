# Copyright 2026 Dolthub, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# simc's tests (the port text's section 13), run in a backend through the
# test module: each test is a TAP test that passes when its outcome is ok.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('cport_simc');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_cport_test');

my $rows = $node->safe_psql('postgres',
	"SELECT name || E'\\t' || replace(outcome, E'\\n', ' ') FROM doltgres_cport_test_simc()"
);
my @expected = (
	'simc::tests::a_body_as_an_actor_runs_with_that_actors_id_in_the_data_directory',
	'simc::tests::a_c_fill_after_a_yield_runs_under_its_own_actors_backend_state',
	'simc::tests::a_c_fills_panic_unwinds_the_simulations_install_with_its_payload',
	'simc::tests::a_c_releases_panic_unwinds_the_owners_abort_with_its_payload',
	'simc::tests::a_checkpoint_sets_the_redo_point_the_host_answers',
	'simc::tests::a_crash_fails_each_call_until_a_recovery_takes_the_files_back_to_their_syncs',
	'simc::tests::a_delta_page_is_taken_written_and_given_back_as_the_simulations_host',
	'simc::tests::a_delta_page_takes_one_of_the_directorys_free_slots',
	'simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c',
	'simc::tests::a_failed_allocation_of_a_listings_names_frees_the_names_made_and_raises_out_of_memory',
	'simc::tests::a_failed_allocation_of_a_panics_payload_frees_its_message_and_raises_out_of_memory',
	'simc::tests::a_failed_std_fs_call_returns_the_operating_systems_error',
	'simc::tests::a_free_pass_is_counted',
	'simc::tests::a_hinted_c_error_crosses_to_sim_as_its_literals',
	'simc::tests::a_historic_snapshot_lends_its_bounds_command_and_sorted_xids',
	'simc::tests::a_hold_of_the_test_process_keeps_its_data_directory_past_its_capability',
	'simc::tests::a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host',
	'simc::tests::a_nap_is_counted_and_the_disk_space_set_is_answered',
	'simc::tests::a_non_vacuumable_snapshot_finds_xids_below_its_horizon_removable',
	'simc::tests::a_pause_a_warning_a_log_and_the_last_redo_answer_as_the_simulations_host',
	'simc::tests::a_pooled_records_doors_answer_as_the_simulations_host',
	'simc::tests::a_processs_flags_are_cleared_before_each_exit_work',
	'simc::tests::a_processs_words_answer_as_the_simulations_host',
	'simc::tests::a_records_slots_answer_as_the_simulations_host',
	'simc::tests::a_refused_open_and_a_refused_record_fail_once_with_the_refusal',
	'simc::tests::a_refused_take_fails_once_and_refuse_nothing_takes_back_the_rest',
	'simc::tests::a_reports_debug_is_the_rusts',
	'simc::tests::a_reports_trace_is_the_rusts',
	'simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts',
	'simc::tests::a_run_check_fails_the_run_at_a_yield_with_its_message',
	'simc::tests::a_run_checks_c_panic_fails_the_run_with_its_message',
	'simc::tests::a_run_dropped_unfinished_drops_its_actors_bodies',
	'simc::tests::a_rust_panic_in_a_host_call_unwinds_the_c_with_its_message',
	'simc::tests::a_scripted_run_runs_each_actor_until_its_point_and_terminates_one',
	'simc::tests::a_shared_cache_of_one_frame_evicts_a_key_for_the_next',
	'simc::tests::a_simulation_counts_traces_and_fills_its_calls',
	'simc::tests::a_sized_delta_answers_its_budget_and_counts_each_hold_begun',
	'simc::tests::a_snapshot_takes_the_simulations_running_xids',
	'simc::tests::a_subtransaction_runs_until_its_abort_under_its_running_parent',
	'simc::tests::a_taken_page_and_a_made_record_count_as_unfreed_until_freed',
	'simc::tests::a_test_directory_the_operating_system_cant_make_fails_with_its_error',
	'simc::tests::a_transactions_status_answers_as_the_simulations_host',
	'simc::tests::a_waiter_at_the_budget_is_counted_and_woken_by_the_end_of_a_hold_that_kept_a_retirement',
	'simc::tests::a_warning_a_log_and_a_kept_wal_read_are_taken_as_the_simulation_took_them',
	'simc::tests::an_actors_c_panic_fails_its_run_with_its_message',
	'simc::tests::an_actors_end_commits_or_aborts_its_transaction_and_starts_a_fresh_one',
	'simc::tests::an_aged_record_unmade_is_given_back_and_a_replacing_page_is_taken',
	'simc::tests::an_exit_work_listed_once_the_exit_began_is_refused',
	'simc::tests::an_exit_work_runs_once_at_its_process_exit',
	'simc::tests::an_open_scratch_file_is_counted_until_it_closes',
	'simc::tests::an_owned_append_refused_after_a_count_fails_once',
	'simc::tests::an_owned_record_is_released_by_its_owners_abort_unless_forgotten',
	'simc::tests::an_unanswered_thread_is_answered_again_once_out_of_recovery',
	'simc::tests::an_unhinted_c_error_crosses_to_sim_as_its_message',
	'simc::tests::each_failing_exit_work_is_in_the_report',
	'simc::tests::each_served_entry_answers_as_the_simulations_host',
	'simc::tests::exit_works_run_newest_first',
	'simc::tests::the_backup_collection_and_database_lock_doors_answer_as_the_simulations_host',
	'simc::tests::the_btree_compare_door_answers_as_the_simulations_host',
	'simc::tests::the_checkpoint_delay_doors_answer_as_the_simulations_host',
	'simc::tests::the_delta_advance_seed_and_pass_doors_answer_as_the_simulations_host',
	'simc::tests::the_delta_hold_and_budget_doors_answer_as_the_simulations_host',
	'simc::tests::the_epoch_doors_answer_as_the_simulations_host',
	'simc::tests::the_injected_door_answers_as_the_simulations_host',
	'simc::tests::the_key_doors_answer_as_the_simulations_host',
	'simc::tests::the_logged_records_are_the_records_the_wal_took',
	'simc::tests::the_new_cid_door_logs_its_record_as_the_simulations_host',
	'simc::tests::the_next_decompression_runs_out_of_memory_when_asked',
	'simc::tests::the_node_cache_installs_finds_and_unpins_a_nodes_frame',
	'simc::tests::the_test_only_std_fs_lists_a_directorys_names',
	'simc::tests::the_test_only_std_fs_makes_a_directory_with_its_parents',
	'simc::tests::the_test_only_std_fs_makes_a_lone_directory_and_refuses_one_that_stands',
	'simc::tests::the_test_only_std_fs_removes_a_directory_with_its_contents',
	'simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file',
	'simc::tests::the_wal_a_checkpoint_and_a_databases_quiet_answer_as_the_simulations_host',
	'simc::tests::the_wal_flush_door_answers_as_the_simulations_host',
	'simc::tests::a_pools_index_answers_as_the_simulations_host',
	'simc::tests::a_flag_try_takes_a_free_flag_and_refuses_its_own',
);
my @tests = split /\n/, $rows;
is_deeply([ map { (split /\t/, $_, 2)[0] } @tests ],
	\@expected, 'the module runs each test it lists');

foreach my $row (@tests)
{
	my ($name, $outcome) = split /\t/, $row, 2;
	is($outcome, 'ok', $name);
}

$node->stop;
done_testing();
