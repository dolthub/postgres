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

# The contract of a process's exit in the server (design item 14's S2),
# through the test module's exit callback, its exit works, and its steps: the
# fork's exit callbacks are registered at a process's start, so a callback
# registered later is cancelled as the newest and runs before the fork's, and
# they take none of the slots stock gives; whether a record is gone is
# answered from its slot, false while a holder keeps a freed record and true
# from then on; and exit works run once each, newest first, at a disconnect
# and at a termination, after the transaction has ended and before the
# process's handles and words are given up, each with no flag held and
# whatever another work returned or raised.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('exit_doors');
$node->init;
$node->append_conf('postgresql.conf', 'log_error_verbosity = verbose');
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

# Runs one step of the test module in a session of its own and returns the
# session's process id, what the step returned, and the error it raised, with
# its SQLSTATE.
sub alone
{
	my ($step, $location, $value) = @_;
	my ($code, $out, $err) = $node->psql('postgres',
		"\\set VERBOSITY verbose\nSELECT pg_backend_pid();\nSELECT doltgres_test_shared($step, $location, $value)"
	);
	my ($pid, $returned) = split /\n/, $out;
	return ($pid, $returned // '', $err);
}

# Attaches an injection point with an action.
sub attach
{
	my ($point, $action) = @_;
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('$point', '$action')");
	return;
}

# Detaches an injection point, so no later backend reaches its action.
sub detach
{
	my ($point) = @_;
	$node->safe_psql('postgres', "SELECT injection_points_detach('$point')");
	return;
}

# Starts a statement in a session and returns once the session has taken it.
sub start
{
	my ($session, $sql) = @_;
	$session->query_until(qr/start/, "\\echo start\n$sql;\n");
	return;
}

# Returns once the backend with a process id has exited, its exit callbacks
# run.
sub exited
{
	my ($pid) = @_;
	$node->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid")
	  or die "backend $pid never exited";
	return;
}

# Starts a session and returns it with its backend's process id.
sub session
{
	my $session = $node->background_psql('postgres', on_error_stop => 0);
	return ($session, $session->query_safe('SELECT pg_backend_pid()'));
}

# Returns the single record's counter, which the exit works add to.
sub counter
{
	return step(26, 0, 0);
}

# Sets the table's four notes, which the exit works write, to zero.
sub clear_notes
{
	step(38, $_, 0) for 0 .. 3;
	return;
}

my $owner = $node->background_psql('postgres');
my $location = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');

# A backend registers a callback as a block that cleans up after an error
# does, uses shared records for the first time inside the block, and cancels
# the callback as the block's end does. The cancel takes only the newest
# callback, which it is, since the fork registered its own at the backend's
# start; and nothing of the block runs at the exit.
my ($pid, $returned, $err) = alone(65, $location, 0);
is("$returned|$err", '0|',
	"a callback registered and cancelled around a backend's first use of shared records raises nothing"
);
exited($pid);
is(step(6, $location, 0),
	'0', 'a cancelled callback is not left to run at the exit');

# A callback registered after the fork's runs before them: it opens a record
# and keeps the handle, and the fork's release, which runs after it, closes
# the record.
($pid, $returned, $err) = alone(66, $location, 1);
is("$returned|$err", '0|', 'a backend registers an exit callback');
exited($pid);
is( step(6, $location, 0),
	'1',
	'a callback registered by a backend runs at its exit and opens a record');
is( step(33, $location, 0),
	'0',
	"a record opened by a callback registered after the fork's is closed at the exit"
);

# The fork's two callbacks sit in two slots of its own: a backend registers
# fifteen callbacks, the twenty slots of stock less the five Postgres has
# registered by its first statement, and the sixteenth finds the list full.
my ($code, $out);
($code, $out, $err) = $node->psql('postgres',
	"\\set VERBOSITY verbose\nSELECT doltgres_test_shared(66, -1, 15);\nSELECT doltgres_test_shared(66, -1, 1)"
);
is($out, '0',
	'a backend registers fifteen exit callbacks, the slots stock leaves it');
like(
	$err,
	qr/FATAL:  54000: out of before_shmem_exit slots/,
	'a backend registers as many exit callbacks as stock leaves it, and one more is out of slots'
);

# Whether a record is gone: not while it is published; at once when it is
# freed with no holder; and a location of another pool is refused.
is(step(63, $location, 0), '0', 'a published record is not gone');
my $lone = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
$owner->query_safe("SELECT doltgres_test_shared(17, $lone, 0)");
$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
is(step(63, $lone, 0), '1', 'a freed record no process held is gone at once');
($code, $out, $err) = $node->psql('postgres',
	"\\set VERBOSITY verbose\nSELECT doltgres_test_shared(63, $location, 1)");
like(
	$err,
	qr/ERROR:  XX000: location \d+ names no record of its shape's pool/,
	"whether a record is gone is refused for a location outside its shape's pool"
);

# A record freed while another backend holds it open is not gone and stays
# reachable to that holder; once the holder closes it is gone, a make takes
# its slot, and it stays gone though its slot holds a published record again.
my $held = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
$owner->query_safe("SELECT doltgres_test_shared(5, $held, 7)");
my $filler = $node->background_psql('postgres');
$filler->query_safe(
	'SELECT count(doltgres_test_shared(19, 0, 0)) FROM generate_series(1, (SELECT doltgres_test_shared(16, 0, 0)))'
);
is(step(16, 0, 0), '0', 'the pool is full');
my $holder = $node->background_psql('postgres');
is($holder->query_safe("SELECT doltgres_test_shared(9, $held, 0)"),
	'1', 'a backend opens a record and keeps it open');
$owner->query_safe("SELECT doltgres_test_shared(17, $held, 0)");
$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
is(step(63, $held, 0),
	'0', 'a freed record another backend holds open is not gone');
is($holder->query_safe('SELECT doltgres_test_shared(64, 0, 0)'),
	'7', 'a freed record stays reachable to the backend that holds it open');
$holder->query_safe('SELECT doltgres_test_shared(10, 0, 0)');
is(step(63, $held, 0),
	'1', 'a freed record is gone once its last holder closes it');
my $made = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
is($made >> 32, $held >> 32, "a make takes a gone record's slot");
is( step(63, $held, 0) . '|' . step(63, $made, 0),
	'1|0',
	'a record stays gone once its slot holds another record, which is not gone'
);
$holder->quit;
$filler->quit;

# A work registered three times runs once, at a disconnect.
my $before = counter();
my ($worker, $worker_pid) = session();
$worker->query_safe('SELECT doltgres_test_shared(67, 0, 1)') for 1 .. 3;
$worker->quit;
exited($worker_pid);
is(counter() - $before,
	1, 'a work registered three times runs once, at a disconnect');

# A work runs with its process's words not yet zeroed and its handles not yet
# given up, outside any transaction; after the works the handles are given up
# and the words zeroed.
clear_notes();
($worker, $worker_pid) = session();
my $proc = $worker->query_safe('SELECT doltgres_test_shared(34, 0, 40)');
$worker->query_safe("SELECT doltgres_test_shared(9, $made, 0)");
$worker->query_safe("SELECT doltgres_test_shared(5, $made, 9)");
$worker->query_safe('SELECT doltgres_test_shared(67, 0, 2)');
$worker->quit;
exited($worker_pid);
is(step(40, 0, 0), '41', "a work reads its process's word, not yet zeroed");
is(step(40, 1, 0),
	'9',
	'a work reaches a record through the handle its process still holds');
is(step(40, 2, 0), '1', 'a work runs outside any transaction');
is(step(35, $proc, 1) . '|' . step(33, $made, 0),
	'0|0',
	"after its works a process's words are zeroed and its record is closed");

# A backend that exits inside a transaction has the transaction aborted
# before its works run.
clear_notes();
($worker, $worker_pid) = session();
$worker->query_safe('BEGIN');
$worker->query_safe('SELECT doltgres_test_shared(67, 0, 2)');
$worker->quit;
exited($worker_pid);
is( step(40, 2, 0),
	'1',
	'a work in a backend that exits inside a transaction finds the transaction ended'
);
is(step(40, 1, 0), '-1',
	'a work in a backend with no record open finds none');

# Works run when their backend is terminated.
$before = counter();
($worker, $worker_pid) = session();
$worker->query_safe('SELECT doltgres_test_shared(67, 0, 1)');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($worker_pid)");
exited($worker_pid);
$worker->{run}->finish;
is(counter() - $before, 1, 'a work runs when its backend is terminated');

# A work that returns an error is logged as a FATAL with its SQLSTATE; the
# work registered before it still runs, and after them the process's record
# is closed and its words zeroed.
$before = counter();
my $offset = -s $node->logfile;
($worker, $worker_pid) = session();
$proc = $worker->query_safe('SELECT doltgres_test_shared(34, 0, 50)');
$worker->query_safe("SELECT doltgres_test_shared(9, $made, 0)");
$worker->query_safe('SELECT doltgres_test_shared(67, 0, 1)');
$worker->query_safe('SELECT doltgres_test_shared(67, 0, 4)');
$worker->quit;
exited($worker_pid);
ok( $node->log_contains(
		qr/FATAL:  22000: the test's exit work failed/, $offset),
	'a work that returns an error is logged as a FATAL with its SQLSTATE');
is(counter() - $before,
	1, "a work that raises doesn't stop the work registered before it");
is( step(35, $proc, 1) . '|' . step(33, $made, 0),
	'0|0',
	"after a work's error its process's words are zeroed and its record is closed"
);

# A work that raises inside a wrapped call, here at its pause, ends the same
# way.
$before = counter();
$offset = -s $node->logfile;
attach('doltgres-test-exit-paused', 'error');
($worker, $worker_pid) = session();
$proc = $worker->query_safe('SELECT doltgres_test_shared(34, 0, 60)');
$worker->query_safe('SELECT doltgres_test_shared(67, 0, 1)');
$worker->query_safe('SELECT doltgres_test_shared(67, 0, 8)');
$worker->quit;
exited($worker_pid);
detach('doltgres-test-exit-paused');
ok( $node->log_contains(
		qr/FATAL:  XX000: error triggered for injection point doltgres-test-exit-paused/,
		$offset),
	'an error raised inside a wrapped call of a work is logged as a FATAL');
is( counter() - $before,
	1,
	"a work that raises inside a wrapped call doesn't finish, and doesn't stop the work registered before it"
);
is(step(35, $proc, 1),
	'0', "after a raise inside a work its process's words are zeroed");

# A backend held at a pause inside its work is seen there, and finishes the
# work once woken.
$before = counter();
attach('doltgres-test-exit-paused', 'wait');
($worker, $worker_pid) = session();
$worker->query_safe('SELECT doltgres_test_shared(67, 0, 8)');
$worker->quit;
$node->wait_for_event('client backend', 'doltgres-test-exit-paused');
is(counter() - $before,
	0, 'a backend held at a pause inside its work has not finished the work');
detach('doltgres-test-exit-paused');
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-test-exit-paused')");
exited($worker_pid);
is(counter() - $before,
	100, 'a backend woken at a pause inside its work finishes the work');

# A registration from inside a work is refused, of a work its backend listed
# and of one it didn't, which so doesn't run. Each of the two works that
# register returns its refusal, which the exit logs.
$before = counter();
$offset = -s $node->logfile;
($worker, $worker_pid) = session();
$worker->query_safe('SELECT doltgres_test_shared(67, 0, 1)');
$worker->query_safe('SELECT doltgres_test_shared(67, 0, 16)');
$worker->query_safe('SELECT doltgres_test_shared(67, 0, 64)');
$worker->quit;
exited($worker_pid);
my @refusals = slurp_file($node->logfile, $offset) =~
  /FATAL:  XX000: an exit work was registered after its process's exit began/g;
is(scalar(@refusals), 2,
	'a registration from inside a work is refused, of a new work and of one already listed'
);
ok( counter() - $before == 1
	  && !$node->log_contains(qr/the test's exit work failed/, $offset),
	'a work refused from inside a work runs neither once more nor at all');

# A backend terminated while it holds a flag leaves the flag held by a frame
# that never returns. Its exit clears the flag before its work, which takes
# the same flag.
$before = counter();
attach('doltgres-shared-gated', 'wait');
($worker, $worker_pid) = session();
$worker->query_safe('SELECT doltgres_test_shared(67, 0, 32)');
start($worker, 'SELECT doltgres_test_shared(29, 0, 0)');
$node->wait_for_event('client backend', 'doltgres-shared-gated');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($worker_pid)");
exited($worker_pid);
$worker->{run}->finish;
detach('doltgres-shared-gated');
is( counter() - $before,
	10000,
	'a backend terminated holding a flag has it cleared before its work, which takes it'
);
$owner->quit;

ok(slurp_file($node->logfile) !~ /PANIC|terminated by signal/,
	'no backend ended in a PANIC');

done_testing();
