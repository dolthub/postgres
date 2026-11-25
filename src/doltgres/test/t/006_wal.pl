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

# The fork's WAL records against their contract (design item 8), through the
# test module: a checkpoint waits while a backend holds off its start, a
# flushed record is durable, pg_walinspect shows the fork's records with their
# kinds and descriptions, redo applies them after a crash, a delay that ends
# unfinished, has its flag cleared, or opens inside another stops the server, a
# record without data is refused, a record of an unknown kind stops recovery,
# a delay outside a transaction is refused, and registering the resource
# manager logs nothing.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('wal');
$node->init;
$node->append_conf('postgresql.conf', 'restart_after_crash = on');
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres',
	'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins(); CREATE EXTENSION injection_points; CREATE EXTENSION pg_walinspect'
);

# A checkpoint waits while a backend holds off its start: the backend holds
# the guard at its injection point, which the guard's open loads since its
# critical section can't allocate, the checkpointer waits for it, and the
# CHECKPOINT finishes once the backend lets go. The backend attaches the point
# itself after setting it local, as Postgres' own tests of a point in a
# critical section do, so the test module's state is set up before the section.
my $holder = $node->background_psql('postgres');
$holder->query_safe(
	"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-delay-checkpoint', 'wait')"
);
$holder->query_until(qr/holding/,
	"\\echo holding\nSELECT doltgres_test_delay(0);\n");
$node->wait_for_event('client backend', 'doltgres-delay-checkpoint');
my $checkpoint = $node->background_psql('postgres');
$checkpoint->query_until(qr/checkpointing/,
	"\\echo checkpointing\nCHECKPOINT;\n");
ok( $node->poll_query_until(
		'postgres',
		"SELECT wait_event = 'CheckpointDelayStart' FROM pg_stat_activity WHERE backend_type = 'checkpointer'"
	),
	'a checkpoint waits for the delay guard');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-delay-checkpoint'); SELECT injection_points_wakeup('doltgres-delay-checkpoint')"
);
is($checkpoint->query_safe('SELECT 1'),
	'1', 'the checkpoint finishes once the guard is released');
$holder->quit;
$checkpoint->quit;

# A flushed record is durable, pg_walinspect describes it, and its redo has not
# run before a crash.
my $start = $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
my $lsn = $node->safe_psql('postgres', 'SELECT doltgres_test_wal(42)');
is( $node->safe_psql(
		'postgres', "SELECT pg_current_wal_flush_lsn() >= '$lsn'::pg_lsn"),
	't',
	'a flushed record is durable');
is( $node->safe_psql(
		'postgres',
		"SELECT resource_manager, record_type, description FROM pg_get_wal_records_info('$start', '$lsn') WHERE resource_manager = 'Versioned'"
	),
	'Versioned|Test|value 42',
	"the fork's records show in pg_walinspect");
is($node->safe_psql('postgres', 'SELECT doltgres_test_load()'),
	'0', "the record's redo has not run before a crash");

# Redo applies the record after a crash.
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', 'SELECT doltgres_test_load()'),
	'42', "redo applies the fork's records after a crash");

# A delay that ends unfinished, as an error leaving it does, stops the server,
# and so do a delay whose flag was cleared inside it and a delay opened inside
# another; the server restarts after each, which its log tells, since a query
# can still succeed in the moment before the postmaster ends the others.
my $offset = -s $node->logfile;
$node->psql('postgres', 'SELECT doltgres_test_delay(1)');
ok( $node->wait_for_log(
		qr/PANIC:  checkpoint delay ended before its change was made visible/,
		$offset),
	'a delay that ends unfinished stops the server');
$node->wait_for_log(qr/database system is ready to accept connections/,
	$offset);
$offset = -s $node->logfile;
$node->psql('postgres', 'SELECT doltgres_test_delay(2)');
ok( $node->wait_for_log(
		qr/PANIC:  checkpoint delay was cleared inside its window/, $offset),
	'a delay whose flag was cleared inside it stops the server');
$node->wait_for_log(qr/database system is ready to accept connections/,
	$offset);
$offset = -s $node->logfile;
$node->psql('postgres', 'SELECT doltgres_test_delay(3)');
ok( $node->wait_for_log(
		qr/PANIC:  cannot delay a checkpoint inside another delay/, $offset),
	'a delay opened inside another stops the server');
$node->wait_for_log(qr/database system is ready to accept connections/,
	$offset);

# A record holds at least one byte, and a record of a kind no crate owns stops
# recovery.
my ($ret, $stdout, $stderr) = $node->psql('postgres',
	"\\set VERBOSITY verbose\nSELECT doltgres_test_wal_unknown(0)");
like(
	$stderr,
	qr/ERROR:  XX000: versioned WAL record of kind \d+ holds no data/,
	'a record without data is refused');
$node->safe_psql('postgres', 'SELECT doltgres_test_wal_unknown(1)');
$node->stop('immediate');
ok(!$node->start(fail_ok => 1), 'a record of an unknown kind stops recovery');
like(
	slurp_file($node->logfile),
	qr/PANIC:  versioned_redo: unknown op code \d+/,
	'it names the unknown op code');

# Redo, outside any transaction, is refused a checkpoint delay, which a
# checkpoint wouldn't wait for.
my $redo = PostgreSQL::Test::Cluster->new('redo_delay');
$redo->init;
$redo->start;
$redo->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
$redo->safe_psql('postgres', 'SELECT doltgres_test_wal(-1)');
$redo->stop('immediate');
ok(!$redo->start(fail_ok => 1),
	'a delay outside a transaction is refused');
like(
	slurp_file($redo->logfile),
	qr/FATAL:  cannot delay a checkpoint outside a transaction/,
	'it says why');

# The fork registers its resource manager without the line Postgres logs for a
# custom one, so the server's log is stock's.
unlike(
	slurp_file($node->logfile),
	qr/registered custom resource manager/,
	'the server log has no line for the resource manager');

# A delay whose work fails with an error stops the server with that error, at
# PANIC with its own SQLSTATE and message, as Postgres raises an error inside a
# critical section, its context naming the delay; the server comes back, and
# redo applies the record the work logged before it failed (bugs row 392).
my $failing = PostgreSQL::Test::Cluster->new('failing_delay');
$failing->init;
$failing->append_conf('postgresql.conf',
	"restart_after_crash = on\nlog_error_verbosity = verbose");
$failing->start;
$failing->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
$offset = -s $failing->logfile;
$failing->psql('postgres', 'SELECT doltgres_test_delay(4)');
ok( $failing->wait_for_log(
		qr/PANIC:  XX001: an error ending a checkpoint delay\n[^\n]*CONTEXT:  checkpoint delay ended before its change was made visible/,
		$offset),
	"a delay whose work fails stops the server with the work's error (row 392)"
);
$failing->wait_for_log(qr/database system is ready to accept connections/,
	$offset);
is($failing->safe_psql('postgres', 'SELECT doltgres_test_load()'),
	'392', 'redo applies the record the failed work logged (row 392)');
$failing->stop;

done_testing();
