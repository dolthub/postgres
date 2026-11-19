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

# A root registration's scope (design item 6's registered roots, bugs row 59):
# a backend terminated inside a registration exits with that frame standing,
# and an entry entered at its exit that registers takes the frame's slots over,
# where one entered outside an exit would be refused.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('registration');
$node->init;
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-test-registered', 'wait')");
my $backend = $node->background_psql('postgres', on_error_stop => 0);
my $pid = $backend->query_safe('SELECT pg_backend_pid()');
$backend->query_until(qr/start/,
	"\\echo start\nSELECT doltgres_test_registered();\n");
$node->wait_for_event('client backend', 'doltgres-test-registered');
my $offset = -s $node->logfile;
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid"),
	'a backend terminated inside a registration exits');
$backend->{run}->finish;
ok( $node->log_contains(
		qr/LOG:  a registration entered at the backend's exit took its root slots/,
		$offset),
	'a backend terminated inside a registration registers at its exit');
ok( !$node->log_contains(
		qr/root slots are taken while an entry has them|TRAP|PANIC/,
		$offset),
	'the registration at its exit is not refused');
is($node->safe_psql('postgres', 'SELECT 1'),
	'1', 'the server stays up after it');

$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-test-registered')");
$node->stop;
done_testing();
