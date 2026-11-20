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

# The binding's cases (design item 17), through dolt_version()'s injection
# points: an error raised inside a wrapped call comes back unchanged and the
# session goes on, a cancel is honored after one, a FATAL inside a wrapped call
# ends only its backend, a panic becomes an internal error, and a failed
# allocation raises out of memory with the backend alive.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('binding');
$node->init;
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

like($node->safe_psql('postgres', 'SELECT dolt_version()'),
	qr/^\d+\.\d+\.\d+$/, 'dolt_version gives the fork\'s version');

my $session = $node->background_psql('postgres', on_error_stop => 0);
$session->query_safe('\set VERBOSITY verbose');
$session->query_safe('SELECT injection_points_set_local()');

# An error raised inside a wrapped call is raised again unchanged.
$session->query_safe(
	"SELECT injection_points_attach('doltgres-version', 'error')");
$session->query('SELECT dolt_version()');
like(
	$session->{stderr},
	qr/ERROR:  XX000: error triggered for injection point doltgres-version/,
	'an error inside a wrapped call comes back unchanged');
$session->{stderr} = '';
$session->query_safe("SELECT injection_points_detach('doltgres-version')");
is($session->query_safe('SELECT 1'), '1', 'the session goes on after it');

# The wrapper restored the interrupt holdoff counts, so a cancel is honored.
$session->query_safe("SET statement_timeout = '500ms'");
$session->query('SELECT pg_sleep(10)');
like(
	$session->{stderr},
	qr/ERROR:  57014: canceling statement due to statement timeout/,
	'a cancel after an error inside a wrapped call is honored');
$session->{stderr} = '';
$session->query_safe('RESET statement_timeout');

# A panic in an entry becomes an internal error.
$session->query_safe(
	"SELECT injection_points_attach('doltgres-version-panic', 'notice')");
$session->query('SELECT dolt_version()');
like(
	$session->{stderr},
	qr/ERROR:  XX000: injected panic in dolt_version/,
	'a panic in an entry is an internal error');
$session->{stderr} = '';
$session->query_safe(
	"SELECT injection_points_detach('doltgres-version-panic')");

# A failed allocation raises out of memory, and the backend stays alive.
$session->query_safe(
	"SELECT injection_points_attach('doltgres-version-out-of-memory', 'notice')"
);
$session->query('SELECT dolt_version()');
like(
	$session->{stderr},
	qr/ERROR:  53200: out of memory\nDETAIL:  Failed on request of size \d+\./,
	'a failed allocation raises out of memory');
$session->{stderr} = '';
$session->query_safe(
	"SELECT injection_points_detach('doltgres-version-out-of-memory')");
like($session->query_safe('SELECT dolt_version()'),
	qr/^\d+\.\d+\.\d+$/, 'the backend works after running out of memory');
$session->quit;

# A FATAL inside a wrapped call ends only its own backend.
my $waiter = $node->background_psql('postgres');
$waiter->query_safe('SELECT injection_points_set_local()');
$waiter->query_safe(
	"SELECT injection_points_attach('doltgres-version-wait', 'wait')");
my $pid = $waiter->query_safe('SELECT pg_backend_pid()');
$waiter->query_until(qr/start/, "\\echo start\nSELECT dolt_version();\n");
$node->wait_for_event('client backend', 'doltgres-version-wait');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
ok( pump_until(
		$waiter->{run}, $waiter->{timeout},
		\$waiter->{stderr},
		qr/terminating connection due to administrator command/),
	'the terminated session gets its FATAL');
$waiter->{run}->finish;
$node->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid")
  or die 'the terminated backend did not exit';
is($node->safe_psql('postgres', 'SELECT 1'),
	'1', 'another session goes on after a FATAL inside a wrapped call');
unlike(slurp_file($node->logfile),
	qr/PANIC|server process .* was terminated by signal/,
	'the FATAL ended only its backend');

# Each reader and writer of a by-value datum's word is the Rust of a
# postgres.h macro: the test module compares each with the macro itself over
# the words and values that tell a wrong cast.
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
is($node->safe_psql('postgres', 'SELECT doltgres_test_words()'),
	'0', 'each word reader and writer gives what its macro gives');

done_testing();
