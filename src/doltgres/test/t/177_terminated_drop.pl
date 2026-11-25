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

# A fold holds its process's interrupts from its drop's mark to the drop's
# end, so a termination sent while it waits there lands after the drop: its
# drop completes, its backend then exits, the server stays up, the next
# checkpoint completes, and the table reads every row once; and a fold that
# fails inside the window gives the hold back, so a later cancel in its
# backend is honored (bugs row 461). Before, the termination left the mark
# set and every later checkpoint on the storage waited for good.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('dropped');
$node->init;
$node->append_conf('postgresql.conf',
	"fold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off");
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION injection_points; CREATE EXTENSION doltgres_test');
my $checkpointer = $node->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE backend_type = 'checkpointer'");

for my $point (
	'doltgres-fold-dropping', 'doltgres-fold-installed',
	'doltgres-drop-installed')
{
	(my $table = $point) =~ s/-/_/g;
	$node->safe_psql('postgres',
		"CREATE TABLE $table (id int4, v text); INSERT INTO $table SELECT g, 'v' FROM generate_series(1, 100) g"
	);
	my $folder = $node->background_psql('postgres', on_error_stop => 0);
	my $pid = $folder->query_safe('SELECT pg_backend_pid()');
	$folder->query_safe(
		"SELECT injection_points_set_local(); SELECT injection_points_attach('$point', 'wait')"
	);
	$folder->query_until(qr/started/,
		"\\echo started\nSELECT doltgres_test_fold('$table');\n");
	$node->wait_for_event('client backend', $point);
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
	is( $node->safe_psql(
			'postgres', "SELECT count(*) FROM pg_stat_activity WHERE pid = $pid"),
		'1',
		"$point: a fold sent a termination inside its drop's window waits on");
	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('$point')");
	ok( $node->poll_query_until(
			'postgres', "SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid"),
		"$point: once woken, the fold's backend exits");
	is( $node->safe_psql(
			'postgres',
			"SELECT split_part(doltgres_test_base_sizes('$table'), ' ', 2)"),
		'100',
		"$point: the fold's drop completed before its backend exited");
	my ($ret, $out, $err) = $node->psql(
		'postgres',
		"SET statement_timeout = '30s'; CHECKPOINT",
		timeout => 60);
	is($ret, 0, "$point: the next checkpoint completes");
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*) || ' ' || count(DISTINCT id) FROM $table"),
		'100 100',
		"$point: the table reads every row once");
}
is( $node->safe_psql(
		'postgres',
		"SELECT pid FROM pg_stat_activity WHERE backend_type = 'checkpointer'"),
	$checkpointer,
	'no termination restarted the server');

# A fold that fails inside its drop's window gives back its hold, so a later
# statement in its backend is still cancelled.
$node->safe_psql('postgres',
	"CREATE TABLE failed (id int4); INSERT INTO failed SELECT generate_series(1, 10)"
);
my $failing = $node->background_psql('postgres', on_error_stop => 0);
my $failing_pid = $failing->query_safe('SELECT pg_backend_pid()');
$failing->query_safe(
	"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-fold-dropping', 'error')"
);
my ($stdout, $stderr) =
  $failing->query("SELECT doltgres_test_fold('failed')");
like($failing->{stderr}, qr/doltgres-fold-dropping/,
	"a fold fails at an error injected inside its drop's window");
$failing->{stderr} = '';
$failing->query_until(qr/slept/, "\\echo slept\nSELECT pg_sleep(60);\n");
$node->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE pid = $failing_pid AND query LIKE 'SELECT pg_sleep%'"
);
$node->safe_psql('postgres', "SELECT pg_cancel_backend($failing_pid)");
$failing->query('SELECT 1');
like($failing->{stderr}, qr/canceling statement due to user request/,
	"after the failed fold a cancel of its backend's next statement is honored");
$failing->quit;

done_testing();
