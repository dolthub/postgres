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

# The contract of host's named pause (design item 14's S2) in the server,
# through two steps of the test module that pause: a wait attached to a
# pause's name holds the process that reaches it, between the two stores the
# pause sits between, until it is woken, and a pause nothing is attached to
# returns at once; a pause under a record's lock holds the process with the
# lock held; and a wait attached to a name no code pauses at holds no process,
# so a case that waits for one there fails by its wait.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('pause');
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

# A backend stores its first word, pauses, and stores its second. With a wait
# attached to the pause's name it is held between the two stores, and goes on
# when woken.
hold('doltgres-test-paused');
my $paused = $node->background_psql('postgres');
my $proc = $paused->query_safe('SELECT doltgres_test_shared(37, 0, 0)');
start($paused, 'SELECT doltgres_test_shared(58, 0, 70)');
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'doltgres-test-paused'"
	),
	"a wait attached to a pause's name holds the process that reaches it");
is( step(35, $proc, 0) . '|' . step(35, $proc, 1),
	'70|0',
	'a process held at a named pause has made its first store and not its second'
);
release('doltgres-test-paused');
pump_until($paused->{run}, $paused->{timeout}, \$paused->{stdout},
	qr/^\d+$/m);
like($paused->{stdout}, qr/^$proc$/m, 'a process woken at its pause goes on');
is(step(35, $proc, 1),
	'70', 'a process woken at its pause makes the store after it');
$paused->quit;

# With nothing attached to its name the pause returns at once.
my $passer = $node->background_psql('postgres');
$proc = $passer->query_safe('SELECT doltgres_test_shared(37, 0, 0)');
is($passer->query_safe('SELECT doltgres_test_shared(58, 0, 80)'),
	$proc, 'a pause with nothing attached to its name returns at once');
is(step(35, $proc, 0) . '|' . step(35, $proc, 1),
	'80|80', 'a process that passed its pause made both stores');

# A pause under a record's lock holds the process with the lock held: another
# backend's take of that lock waits until the pause is woken.
my $owner = $node->background_psql('postgres');
my $location = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
hold('doltgres-test-paused-locked');
my $locker = $node->background_psql('postgres');
start($locker, "SELECT doltgres_test_shared(59, $location, 0)");
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'doltgres-test-paused-locked'"
	),
	"a wait attached to a pause's name holds the process that reaches it under a lock");
my $reader = $node->background_psql('postgres');
start($reader, "SELECT doltgres_test_shared(2, $location, 0)");
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event_type = 'LWLock' AND wait_event = 'versioned shared memory'"
	),
	"a pause under a record's lock holds the process with the lock held");
release('doltgres-test-paused-locked');
pump_until($reader->{run}, $reader->{timeout}, \$reader->{stdout},
	qr/^\d+$/m);
like($reader->{stdout}, qr/^0$/m,
	"the lock's waiter goes on once the pause under the lock is woken");
$locker->quit;
$reader->quit;
$owner->quit;

# A wait attached to a name no code pauses at holds no process: the step
# passes its own pause, and a case that waits for a process at that name,
# as every case waits before it goes on, fails by its wait.
hold('doltgres-test-nowhere');
is($passer->query_safe('SELECT doltgres_test_shared(58, 0, 90)'),
	$proc, 'a process passes its pause while a wait is attached to another name');
{
	local $PostgreSQL::Test::Utils::timeout_default = 2;
	my $found = $node->poll_query_until('postgres',
		"SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'doltgres-test-nowhere'"
	);
	ok(!$found,
		'a wait attached to a name no code pauses at holds no process, and the wait for one there fails'
	);
}
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-test-nowhere')");
$passer->quit;

ok(slurp_file($node->logfile) !~ /PANIC|terminated by signal/,
	'no backend ended in a PANIC');

done_testing();
