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

# The server's host against its contract (design item 14): through the test
# module, the host's process id is the backend's, its time of day is the
# server's clock, its unique numbers increase, its monotonic clock advances
# across a sleep, a long loop's check stops it once a cancel is pending, and an
# entry after a panic under a static lock rebuilds its state and works.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('host');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

is( $node->safe_psql(
		'postgres', 'SELECT doltgres_test_host(0) = pg_backend_pid()'),
	't',
	"the host's process id is the backend's");
is( $node->safe_psql(
		'postgres',
		"SELECT abs(doltgres_test_host(1) - (extract(epoch FROM clock_timestamp()) * 1000000)::int8) < 5000000"
	),
	't',
	"the host's time of day is the server's clock");
is( $node->safe_psql(
		'postgres',
		'SELECT doltgres_test_host(2) < doltgres_test_host(2) AND doltgres_test_host(2) < doltgres_test_host(2)'),
	't',
	"the host's unique numbers increase");
is( $node->safe_psql(
		'postgres',
		'SELECT doltgres_test_host(3) - before >= 100000000 FROM (SELECT doltgres_test_host(3) AS before, pg_sleep(0.1) OFFSET 0) s'
	),
	't',
	"the host's monotonic clock advances across a sleep");

# A long loop's check stops it once an interrupt is pending (L2).
my ($ret, $stdout, $stderr) = $node->psql('postgres',
	"\\set VERBOSITY verbose\nSET statement_timeout = '100ms';\nSELECT doltgres_test_check()");
like(
	$stderr,
	qr/ERROR:  57014: canceling statement due to statement timeout/,
	'a pending cancel stops a loop at its next check');

# A panic while a static storage lock is held poisons it, and the next entry
# rebuilds its state instead of failing for the rest of the session (B5).
my $session = $node->background_psql('postgres', on_error_stop => 0);
$session->query_safe('\\set VERBOSITY verbose');
is($session->query_safe('SELECT doltgres_test_lock(0)'),
	'1', "the backend's counter counts under its lock");
$session->query('SELECT doltgres_test_lock(1)');
like(
	$session->{stderr},
	qr/ERROR:  XX000: a step's bug while the counter's lock is held/,
	'a panic under the lock is an internal error');
$session->{stderr} = '';
is($session->query_safe('SELECT doltgres_test_lock(0)'),
	'1', 'an entry after a panic under a static lock works');
$session->quit;

# The host's last redo point is the control file's, which a completed checkpoint
# moves.
$node->safe_psql('postgres', 'CREATE TABLE redo_moves (i int); CHECKPOINT');
is( $node->safe_psql(
		'postgres',
		"SELECT '0/0'::pg_lsn + doltgres_test_host(4) = redo_lsn FROM pg_control_checkpoint()"
	),
	't',
	"the host's last redo point is the last checkpoint's");

# The host's session database is the backend's: its database's OID and the
# tablespace of its own directory, here one made in another tablespace.
my $space = $node->basedir . '/session_space';
mkdir($space);
$node->safe_psql('postgres',
	"CREATE TABLESPACE session_space LOCATION '$space'");
$node->safe_psql('postgres',
	'CREATE DATABASE elsewhere TABLESPACE session_space');
$node->safe_psql('elsewhere', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
is( $node->safe_psql(
		'elsewhere',
		'SELECT doltgres_test_host(7) = oid::int8 AND doltgres_test_host(8) = dattablespace::int8 FROM pg_database WHERE datname = current_database()'
	),
	't',
	"the host's session database is the backend's");

done_testing();
