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

# The contract of a later long lock taken under a long lock of the same
# record (design item 14's S2) in the server, through the test module's steps
# over a pooled record's barrier and its later long lock: two backends hold
# the barrier shared, and the one that asks for the later lock exclusively
# waits while the other holds it, exclusively or shared, and takes it once
# the other lets go; a later long lock held shared is taken shared by another
# backend at once.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('long_locks');
$node->init;
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

# Runs one step of the test module in a session of its own.
sub step
{
	my ($step, $location, $value) = @_;
	return $node->safe_psql('postgres',
		"SELECT doltgres_test_shared($step, $location, $value)");
}

# Attaches an injection point that waits.
sub hold
{
	my ($point) = @_;
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('$point', 'wait')");
	return;
}

# Detaches an injection point and wakes its one waiter.
sub release
{
	my ($point) = @_;
	$node->safe_psql('postgres', "SELECT injection_points_detach('$point')");
	$node->safe_psql('postgres', "SELECT injection_points_wakeup('$point')");
	return;
}

# Starts a statement in a session and returns once the session has taken it.
sub start
{
	my ($session, $sql) = @_;
	$session->query_until(qr/start/, "\\echo start\n$sql;\n");
	return;
}

# Returns whether one backend waits at the injection point and a number wait
# for a record's lock.
sub waiting
{
	my ($locked) = @_;
	return $node->poll_query_until('postgres',
		"SELECT count(*) FILTER (WHERE wait_event = 'doltgres-shared-listed') = 1 AND count(*) FILTER (WHERE wait_event_type = 'LWLock' AND wait_event = 'versioned shared memory') = $locked FROM pg_stat_activity"
	);
}

my $owner = $node->background_psql('postgres');
my $location = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
is(step(61, $location, 0) . step(62, $location, 0),
	'00', 'a later long lock is taken under a long lock, exclusively and shared');

# A backend holds the barrier shared and the later long lock exclusively,
# paused; a second backend takes the barrier shared at once and waits for the
# later lock until the first lets go.
hold('doltgres-shared-listed');
my $holder = $node->background_psql('postgres');
start($holder, "SELECT doltgres_test_shared(60, $location, 0)");
ok(waiting(0),
	'a backend holds a long lock shared and a later long lock under it');
my $taker = $node->background_psql('postgres');
start($taker, "SELECT doltgres_test_shared(61, $location, 0)");
ok(waiting(1),
	'a second holder of the outer lock waits for the later long lock the first holds');
release('doltgres-shared-listed');
pump_until($taker->{run}, $taker->{timeout}, \$taker->{stdout}, qr/^\d+$/m);
like($taker->{stdout}, qr/^0$/m,
	'the waiter takes the later long lock once its holder lets go');
$holder->quit;
$taker->quit;

# A backend holds the later long lock shared, paused; another takes it shared
# at once, and one that asks for it exclusively waits until the first lets go.
hold('doltgres-shared-listed');
$holder = $node->background_psql('postgres');
start($holder, "SELECT doltgres_test_shared(60, $location, 1)");
ok(waiting(0), 'a backend holds a later long lock shared under a long lock');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-shared-listed')");
my $sharer = $node->background_psql('postgres');
start($sharer, "SELECT doltgres_test_shared(62, $location, 0)");
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE state = 'idle' AND query LIKE '%doltgres_test_shared(62,%'"
	),
	'a later long lock held shared is taken shared by another backend');
$taker = $node->background_psql('postgres');
start($taker, "SELECT doltgres_test_shared(61, $location, 0)");
ok(waiting(1),
	'a backend that asks for a later long lock exclusively waits for its shared holder'
);
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-shared-listed')");
pump_until($taker->{run}, $taker->{timeout}, \$taker->{stdout}, qr/^\d+$/m);
like($taker->{stdout}, qr/^0$/m,
	'the exclusive take gets the later long lock once its shared holder lets go'
);
$holder->quit;
$sharer->quit;
$taker->quit;
$owner->quit;

ok(slurp_file($node->logfile) !~ /PANIC|terminated by signal/,
	'no backend ended in a PANIC');

done_testing();
