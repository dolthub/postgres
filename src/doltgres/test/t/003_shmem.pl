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

# The fork's shared memory (design item 6), through the test module's functions
# and injection points that force the interleavings: the region is listed, a
# backend that holds the fork's lock makes another wait and then see what it
# stored, an error under the lock leaves the session's interrupts balanced,
# backends that took the lock exit cleanly, the lock's measurement runs each
# way, an error inside an escalating wrapper becomes a PANIC, and extensions
# keep every LWLock tranche stock gives them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('shmem');
$node->init;
$node->append_conf('postgresql.conf', 'restart_after_crash = on');
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_shmem_allocations WHERE name = 'versioned shared memory'"
	),
	'1',
	'the versioned shared memory region is listed');

# One backend stores while holding the lock, paused at an injection point; a
# second backend's load waits on the lock, then sees the stored value.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-shmem-store', 'wait')");
my $holder = $node->background_psql('postgres');
$holder->set_query_timer_restart;
$holder->query_until(qr/start/,
	"\\echo start\nSELECT doltgres_test_store(42);\n");
$node->wait_for_event('client backend', 'doltgres-shmem-store');
my $reader = $node->background_psql('postgres');
$reader->set_query_timer_restart;
$reader->query_until(qr/start/,
	"\\echo start\nSELECT doltgres_test_load();\n");
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event_type = 'LWLock' AND wait_event = 'versioned shared memory'"
	),
	"the second backend waits on the fork's lock");
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-shmem-store')");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-shmem-store')");
$reader->{timeout}->start;
ok( pump_until(
		$reader->{run}, $reader->{timeout}, \$reader->{stdout}, qr/^42$/m),
	"a backend waiting on the fork's lock sees the value the holder stored");
$_->{timeout}->start for ($holder, $reader);
$holder->quit;
$reader->quit;

# An error raised inside a wrapped call made under the fork's lock unwinds the
# guard, which releases the lock and its interrupt holdoff, so a later cancel
# in the same session is honored.
my $session = $node->background_psql('postgres', on_error_stop => 0);
$session->set_query_timer_restart;
$session->query_safe('SELECT injection_points_set_local()');
$session->query_safe(
	"SELECT injection_points_attach('doltgres-shmem-store', 'error')");
$session->query('SELECT doltgres_test_store(7)');
like(
	$session->{stderr},
	qr/error triggered for injection point doltgres-shmem-store/,
	"an error under the fork's lock returns to the session");
$session->{stderr} = '';
my $pid = $session->query_safe('SELECT pg_backend_pid()');
$session->query_until(qr/start/, "\\echo start\nSELECT pg_sleep(600);\n");
$node->safe_psql('postgres', "SELECT pg_cancel_backend($pid)");
ok( pump_until(
		$session->{run}, $session->{timeout},
		\$session->{stderr}, qr/canceling statement due to user request/),
	"a cancel after an error under the fork's lock is honored");
$session->{timeout}->start;
$session->quit;

# Backends that took the fork's lock exit cleanly.
ok( $node->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'client backend' AND pid <> pg_backend_pid()"
	  )
	  && slurp_file($node->logfile) !~ /PANIC|terminated by signal/,
	"backends that took the fork's lock exit cleanly");

is( $node->safe_psql(
		'postgres',
		'SELECT string_agg((doltgres_test_lock_pairs(1000, way) >= 0)::text, \',\' ORDER BY way) FROM generate_series(0, 2) way'
	),
	'true,true,true',
	'the measurement runs each way');

# An error inside an escalating wrapper, such as the lock's release, can't
# unwind into Rust: it becomes a PANIC, and the server restarts.
my $offset = -s $node->logfile;
$node->psql('postgres', 'SELECT doltgres_test_escalate()');
ok( $node->wait_for_log(
		qr/database system is ready to accept connections/, $offset),
	'the server restarts after the PANIC');
like(
	slurp_file($node->logfile),
	qr/PANIC:  an error inside an escalating wrapper/,
	'an error inside an escalating wrapper becomes a PANIC');

# Extensions keep every LWLock tranche stock Postgres gives them, and no more:
# test_lwlock_tranches, preloaded, takes two with RequestNamedLWLockTranche,
# so 254 more can be created, and the next is refused with stock's limit.
my $tranches = PostgreSQL::Test::Cluster->new('tranches');
$tranches->init;
$tranches->append_conf('postgresql.conf',
	"shared_preload_libraries = 'test_lwlock_tranches'");
$tranches->start;
$tranches->safe_psql('postgres', 'CREATE EXTENSION test_lwlock_tranches');
my ($ret, $out, $err) = $tranches->psql('postgres',
	"SELECT count(test_lwlock_tranche_create('consume_' || i)) FROM generate_series(1, 254) i; SELECT test_lwlock_tranche_create('one too many')"
);
ok( $out eq '254'
	  && $err =~
	  /maximum number of tranches already registered\nDETAIL:  No more than 256 tranches may be registered\./,
	'extensions keep every tranche stock gives');

done_testing();
