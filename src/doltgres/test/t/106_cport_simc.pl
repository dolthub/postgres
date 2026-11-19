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
	'simc::tests::a_checkpoint_sets_the_redo_point_the_host_answers',
	'simc::tests::a_crash_fails_each_call_until_a_recovery_takes_the_files_back_to_their_syncs',
	'simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c',
	'simc::tests::a_failed_allocation_of_a_listings_names_frees_the_names_made_and_raises_out_of_memory',
	'simc::tests::a_failed_allocation_of_a_panics_payload_frees_its_message_and_raises_out_of_memory',
	'simc::tests::a_failed_std_fs_call_returns_the_operating_systems_error',
	'simc::tests::a_hinted_c_error_crosses_to_sim_as_its_literals',
	'simc::tests::a_pause_a_warning_a_log_and_the_last_redo_answer_as_the_simulations_host',
	'simc::tests::a_pooled_records_doors_answer_as_the_simulations_host',
	'simc::tests::a_processs_flags_are_cleared_before_each_exit_work',
	'simc::tests::a_processs_words_answer_as_the_simulations_host',
	'simc::tests::a_records_slots_answer_as_the_simulations_host',
	'simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts',
	'simc::tests::a_run_dropped_unfinished_drops_its_actors_bodies',
	'simc::tests::a_rust_panic_in_a_host_call_unwinds_the_c_with_its_message',
	'simc::tests::a_scripted_run_runs_each_actor_until_its_point_and_terminates_one',
	'simc::tests::a_simulation_counts_traces_and_fills_its_calls',
	'simc::tests::a_test_directory_the_operating_system_cant_make_fails_with_its_error',
	'simc::tests::a_warning_a_log_and_a_kept_wal_read_are_taken_as_the_simulation_took_them',
	'simc::tests::an_actors_c_panic_fails_its_run_with_its_message',
	'simc::tests::an_exit_work_listed_once_the_exit_began_is_refused',
	'simc::tests::an_exit_work_runs_once_at_its_process_exit',
	'simc::tests::an_open_scratch_file_is_counted_until_it_closes',
	'simc::tests::an_unhinted_c_error_crosses_to_sim_as_its_message',
	'simc::tests::each_failing_exit_work_is_in_the_report',
	'simc::tests::each_served_entry_answers_as_the_simulations_host',
	'simc::tests::exit_works_run_newest_first',
	'simc::tests::the_epoch_doors_answer_as_the_simulations_host',
	'simc::tests::the_next_decompression_runs_out_of_memory_when_asked',
	'simc::tests::the_node_cache_installs_finds_and_unpins_a_nodes_frame',
	'simc::tests::the_test_only_std_fs_lists_a_directorys_names',
	'simc::tests::the_test_only_std_fs_makes_a_directory_with_its_parents',
	'simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file',
	'simc::tests::the_wal_a_checkpoint_and_a_databases_quiet_answer_as_the_simulations_host',
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
