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

# The contract of host's shared records (design item 14's S2) in the server,
# through the test module's steps over a pool of its own shape, a second pool,
# and a single record, with injection points pausing a backend where a case
# needs it: the region is listed at the size its kinds take and a pool's count
# follows the server's processes; a record's blob, lock, atomic, and flag work
# across backends, and a flag's take that fails sets no flag; a full pool
# refuses a make with an internal error naming its kind, and takes one at once
# after a free, in a slot that reads zeroed; a
# make that fails takes no slot; a free takes nothing and can't fail; a forged
# location and another pool's are refused, and a stale one answers nothing; a
# record freed while held goes at its last close; a reclaim frees a record
# only if no process opened or claimed it since; makes that race for idle
# records each get one; a terminated backend's handles are given up; a make
# after a free waits for no reader; a slot freed at its last generation is
# spent; and a FATAL under a record's lock leaves a slot that is made again.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('shared');
$node->init;
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

# Runs one step of the test module in a session of its own, which gives up at
# its exit whatever handle the step kept.
sub step
{
	my ($server, $step, $location, $value) = @_;
	return $server->safe_psql('postgres',
		"SELECT doltgres_test_shared($step, $location, $value)");
}

# Returns the free slots of the test pool.
sub free_slots
{
	my ($server) = @_;
	return step($server, 16, 0, 0);
}

# Attaches an injection point that waits.
sub hold
{
	my ($server, $point) = @_;
	$server->safe_psql('postgres',
		"SELECT injection_points_attach('$point', 'wait')");
	return;
}

# Detaches an injection point and wakes its waiter.
sub release
{
	my ($server, $point) = @_;
	$server->safe_psql('postgres',
		"SELECT injection_points_detach('$point')");
	$server->safe_psql('postgres',
		"SELECT injection_points_wakeup('$point')");
	return;
}

# Starts a statement in a session and returns once the session has taken it.
sub start
{
	my ($session, $sql) = @_;
	$session->query_until(qr/start/, "\\echo start\n$sql;\n");
	return;
}

my $count = step($node, 31, 0, 0);
is( $node->safe_psql(
		'postgres',
		"SELECT size = doltgres_test_shared(32, 0, 0) FROM pg_shmem_allocations WHERE name = 'versioned shared memory'"
	),
	't',
	'the region is listed at the size its kinds take');
is(free_slots($node), $count, 'every slot of a pool is free at first');

# Each record is made and published, then claimed and freed, by one owner
# session.
my $owner = $node->background_psql('postgres', on_error_stop => 0);
my $location = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
step($node, 1, $location, 42);
is(step($node, 2, $location, 0),
	'42', 'a blob written by one backend is read by another');

# A backend holds the record's lock, paused at an injection point; another
# backend's read waits on it, and then sees the blob.
hold($node, 'doltgres-shared-locked');
my $holder = $node->background_psql('postgres');
start($holder, "SELECT doltgres_test_shared(3, $location, 0)");
$node->wait_for_event('client backend', 'doltgres-shared-locked');
my $reader = $node->background_psql('postgres');
start($reader, "SELECT doltgres_test_shared(2, $location, 0)");
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event_type = 'LWLock' AND wait_event = 'versioned shared memory'"
	),
	"a backend waits on another's hold of a record's lock");
release($node, 'doltgres-shared-locked');
ok( pump_until(
		$reader->{run}, $reader->{timeout},
		\$reader->{stdout}, qr/^42$/m),
	'the waiting backend reads the blob once the lock is released');
$holder->quit;
$reader->quit;
my ($code, $out, $err) =
  $node->psql('postgres', "SELECT doltgres_test_shared(4, $location, 0)");
like($err, qr/a backend took a shared lock it already holds/,
	'a second take of a held lock is refused');

($code, $out, $err) = $node->psql('postgres',
	"\\set VERBOSITY verbose\nSELECT doltgres_test_shared(15, $location, 17)");
like(
	$err,
	qr/ERROR:  XX000: a shared blob's bytes are longer than its largest size/,
	"bytes one past a blob's largest size are refused");
is(step($node, 2, $location, 0),
	'42',
	"bytes one past a blob's largest size are refused and the next read gives the bytes before"
);

is(step($node, 5, $location, 5), '0', 'an add returns the atomic before it');
is(step($node, 6, $location, 0),
	'5', 'an atomic changed by one backend is read by another');
is( $owner->query_safe("SELECT doltgres_test_shared(6, $location, 0)"),
	'5',
	'a record made by one backend is opened by another, and an atomic it changed is read there'
);

# A backend holds the record's flag, paused at an injection point; another
# backend waits for it with the fork's wait event until a cancel ends the wait.
hold($node, 'doltgres-shared-flagged');
$holder = $node->background_psql('postgres');
start($holder, "SELECT doltgres_test_shared(7, $location, 0)");
$node->wait_for_event('client backend', 'doltgres-shared-flagged');
my $waiter = $node->background_psql('postgres', on_error_stop => 0);
start($waiter, "SELECT doltgres_test_shared(8, $location, 0)");
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event_type = 'Extension' AND wait_event = 'versioned flag'"
	),
	"a backend waits for another's flag");
$node->safe_psql('postgres',
	"SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE wait_event = 'versioned flag'"
);
ok( pump_until(
		$waiter->{run}, $waiter->{timeout},
		\$waiter->{stderr}, qr/canceling statement due to user request/),
	'a flag another backend holds is waited for and a cancel ends the wait');
release($node, 'doltgres-shared-flagged');
$holder->quit;
$waiter->quit;
is(step($node, 8, $location, 0),
	'0', 'the flag is free once its holder ends');

# A take of a flag that fails at its list entry, as an allocation failure makes
# it, leaves the flag clear, however often, and another backend then takes the
# flag at once.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-shared-flag-refused', 'notice')"
);
($code, $out, $err) = $node->psql(
	'postgres', "
DO \$\$
DECLARE
	refused int := 0;
BEGIN
	FOR i IN 1..20000 LOOP
		BEGIN
			PERFORM doltgres_test_shared(8, $location, 0);
		EXCEPTION WHEN out_of_memory THEN
			refused := refused + 1;
		END;
	END LOOP;
	RAISE NOTICE 'refused %', refused;
END
\$\$");
like($err, qr/refused 20000/,
	"a flag's take refused at its list entry fails");
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-shared-flag-refused')");
($code, $out, $err) = $node->psql('postgres',
	"SET statement_timeout = '10s'; SELECT doltgres_test_shared(8, $location, 0)"
);
is($out, '0',
	"a flag's take refused at its list entry leaves the flag clear, and another backend takes it at once"
);

# The single record is reached by every backend with no location.
is(step($node, 25, 0, 7),
	'0', "an add returns the single record's atomic before it");
is(step($node, 26, 0, 0),
	'7', "a single record's atomic changed by one backend is read by another");

# A pool takes its count of makes, and the next fails with an internal error
# naming the kind, changing nothing.
my $filler = $node->background_psql('postgres', on_error_stop => 0);
my $rest = $count - 1;
is( $filler->query_safe(
		"SELECT count(doltgres_test_shared(19, 0, 0)) FROM generate_series(1, $rest)"
	),
	$rest,
	'a pool takes its count of makes');
is(free_slots($node), '0', 'a full pool has no free slot');
($code, $out, $err) = $node->psql('postgres',
	"\\set VERBOSITY verbose\nSELECT doltgres_test_shared(19, 0, 0)");
like(
	$err,
	qr/ERROR:  XX000: every test record is in use/,
	'a full pool refuses a make with an internal error naming the kind');
is(free_slots($node), '0', 'a refused make changes nothing');

# After a free the next make succeeds at once, in the freed slot, which reads
# zeroed though the record before it held an atomic and a blob.
is($owner->query_safe("SELECT doltgres_test_shared(17, $location, 0)"),
	'1', 'a published record is claimed');
$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
is(free_slots($node), '1', 'a free with no holder frees the slot at once');
is(step($node, 9, $location, 0),
	'0', "a freed record's location answers nothing");
my $again = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
is( $node->safe_psql(
		'postgres',
		"SELECT doltgres_test_shared(6, $again, 0), doltgres_test_shared(2, $again, 0), doltgres_test_shared(8, $again, 0)"
	),
	'0|0|0',
	'a record made in a freed slot reads zeroed');
is(step($node, 9, $location, 0),
	'0',
	"a freed record's location answers nothing once its slot holds another record"
);

# A make that fails at its list entry, as an allocation failure makes it, takes
# no slot, however often.
$filler->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
is(free_slots($node), '1', 'a backend frees a record it owns');
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-shared-make-refused', 'notice')"
);
($code, $out, $err) = $node->psql(
	'postgres', q{
DO $$
DECLARE
	refused int := 0;
BEGIN
	FOR i IN 1..20000 LOOP
		BEGIN
			PERFORM doltgres_test_shared(19, 0, 0);
		EXCEPTION WHEN out_of_memory THEN
			refused := refused + 1;
		END;
	END LOOP;
	RAISE NOTICE 'refused %', refused;
END
$$});
like($err, qr/refused 20000/, 'a make refused at its list entry fails');
is(free_slots($node), '1',
	'a make refused at its list entry leaves every slot as it was');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-shared-make-refused')");
$filler->query_safe('SELECT doltgres_test_shared(19, 0, 0)');
is(free_slots($node), '0',
	'a make succeeds after twenty thousand refused makes');

# With every slot taken, a backend frees every record it owns with no error,
# and a terminated one leaves its unpublished records free at its exit, with
# no checkpoint.
is( $filler->query_safe(
		"SELECT count(doltgres_test_shared(14, 0, 0)) FROM generate_series(1, $rest)"
	),
	$rest,
	'with every slot taken, a backend frees every record it owns');
is(free_slots($node), $rest, 'a free takes nothing and gives the slot back');
my $doomed = $node->background_psql('postgres', on_error_stop => 0);
$doomed->query_safe(
	"SELECT count(doltgres_test_shared(19, 0, 0)) FROM generate_series(1, $rest)"
);
is(free_slots($node), '0', "the pool is full of a backend's records");
my $pid = $doomed->query_safe('SELECT pg_backend_pid()');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
ok( $node->poll_query_until(
		'postgres', "SELECT doltgres_test_shared(16, 0, 0) = $rest"),
	'a terminated backend leaves its unpublished records free at its exit');
$doomed->{run}->finish;

# Two backends make with one slot free: the first loads the slot's word and
# pauses before its compare-and-swap, the second takes the slot, and the first
# then finds the pool full.
my $spare = $rest - 1;
$filler->query_safe(
	"SELECT count(doltgres_test_shared(19, 0, 0)) FROM generate_series(1, $spare)"
);
is(free_slots($node), '1', 'one slot is free');
my $slow = $node->background_psql('postgres', on_error_stop => 0);
$slow->query_safe('SELECT injection_points_set_local()');
$slow->query_safe(
	"SELECT injection_points_attach('doltgres-shared-making', 'wait')");
$slow->query_safe('\set VERBOSITY verbose');
start($slow, 'SELECT doltgres_test_shared(19, 0, 0)');
$node->wait_for_event('client backend', 'doltgres-shared-making');
$filler->query_safe('SELECT doltgres_test_shared(19, 0, 0)');
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-shared-making')");
ok( pump_until(
		$slow->{run}, $slow->{timeout},
		\$slow->{stderr}, qr/XX000: every test record is in use/),
	'two backends making until the pool is full get no slot twice');
is(free_slots($node), '0', 'the pool holds its count of records');
$slow->quit;
$filler->query_safe(
	"SELECT count(doltgres_test_shared(14, 0, 0)) FROM generate_series(1, $rest)"
);

# A forged location and another pool's are refused by every door that takes a
# location, and one of another generation answers nothing.
my $forged = '(1000000::bigint << 32)';
for my $door (9, 17, 21)
{
	($code, $out, $err) = $node->psql('postgres',
		"\\set VERBOSITY verbose\nSELECT doltgres_test_shared($door, $forged, 0)"
	);
	like(
		$err,
		qr/ERROR:  XX000: location \d+ names no record of its shape's pool/,
		'a forged location is refused');
}
for my $door (0, 1, 2)
{
	($code, $out, $err) = $node->psql('postgres',
		"\\set VERBOSITY verbose\nSELECT doltgres_test_shared(27, $again, $door)"
	);
	like(
		$err,
		qr/ERROR:  XX000: location \d+ names no record of its shape's pool/,
		"a location of another kind's pool is refused");
}
is( $node->safe_psql(
		'postgres',
		"SELECT doltgres_test_shared(9, $again + 1, 0), doltgres_test_shared(17, $again + 1, 0), doltgres_test_shared(21, $again + 1, 0)"
	),
	'0|0|0',
	'a location of another generation answers nothing');

# A record freed while a backend holds it open takes no new open, is the
# holder's until its close, and is gone then.
my $before = free_slots($node);
hold($node, 'doltgres-shared-locked');
$holder = $node->background_psql('postgres');
start($holder, "SELECT doltgres_test_shared(3, $again, 0)");
$node->wait_for_event('client backend', 'doltgres-shared-locked');
is($owner->query_safe("SELECT doltgres_test_shared(17, $again, 0)"),
	'1', 'a record a backend holds open is claimed');
$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
is(step($node, 9, $again, 0),
	'0', 'a record freed while held takes no new open');
is(free_slots($node), $before, 'a record freed while held keeps its slot');
release($node, 'doltgres-shared-locked');
ok( pump_until(
		$holder->{run}, $holder->{timeout},
		\$holder->{stdout}, qr/^0$/m),
	'an open that won before a free keeps the record until its close');
$holder->quit;
ok( $node->poll_query_until(
		'postgres', "SELECT doltgres_test_shared(16, 0, 0) = $before + 1"),
	"the holder's close frees the slot");

# An open paused before its compare-and-swap, with the record freed meanwhile,
# answers nothing.
my $gone = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
$slow = $node->background_psql('postgres', on_error_stop => 0);
$slow->query_safe('SELECT injection_points_set_local()');
$slow->query_safe(
	"SELECT injection_points_attach('doltgres-shared-opening', 'wait')");
start($slow, "SELECT doltgres_test_shared(9, $gone, 0)");
$node->wait_for_event('client backend', 'doltgres-shared-opening');
$owner->query_safe("SELECT doltgres_test_shared(17, $gone, 0)");
$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-shared-opening')");
ok( pump_until(
		$slow->{run}, $slow->{timeout},
		\$slow->{stdout}, qr/^0$/m),
	'an open begun before a free and finished after it answers nothing');
$slow->quit;

# A reclaim takes a published record no process holds, and frees it only if
# no process opened or claimed it since.
my $idle = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
my $looker = $node->background_psql('postgres', on_error_stop => 0);
is($looker->query_safe("SELECT doltgres_test_shared(21, $idle, 0)"),
	'1', 'a reclaim takes a published record no process holds');
is(step($node, 21, $idle, 0),
	'2',
	'a second reclaim of a record a reclaim holds is told another is looking');
step($node, 5, $idle, 3);
is($looker->query_safe('SELECT doltgres_test_shared(22, 0, 0)'),
	'0', "a reclaim's free fails once a process opened the record");
is(step($node, 6, $idle, 0),
	'3', 'and the record is there with what the process changed');
step($node, 5, $idle, -3);
$looker->query_safe("SELECT doltgres_test_shared(21, $idle, 0)");
$node->psql('postgres', "SELECT doltgres_test_shared(18, $idle, 0)");
is($looker->query_safe('SELECT doltgres_test_shared(22, 0, 0)'),
	'0',
	"a reclaim's free fails once a process claimed the record and dropped the claim"
);
($code, $out, $err) =
  $node->psql('postgres', "SELECT doltgres_test_shared(9, $idle, 0)");
is($out, '1',
	'a claim an error dropped under a reclaim leaves the record published');
$holder = $node->background_psql('postgres');
$holder->query_safe("SELECT doltgres_test_shared(9, $idle, 0)");
is(step($node, 21, $idle, 0),
	'0', 'a reclaim of a record a backend holds answers that it is in use');
$holder->query_safe('SELECT doltgres_test_shared(10, 0, 0)');
$holder->quit;
$before = free_slots($node);
$looker->query_safe("SELECT doltgres_test_shared(21, $idle, 0)");
is($owner->query_safe("SELECT doltgres_test_shared(17, $idle, 0)"),
	'1', 'a record a reclaim looks at is claimed');
$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
is(free_slots($node), $before,
	'a record freed while a reclaim looks at it keeps its slot');
is($looker->query_safe('SELECT doltgres_test_shared(22, 0, 0)'),
	'0', "the reclaim's free of a record freed meanwhile answers false");
is(free_slots($node), $before + 1,
	'a record claimed and freed while a reclaim looks at it is free once the reclaim gives it up'
);
$idle = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
$looker->query_safe("SELECT doltgres_test_shared(21, $idle, 0)");
is($looker->query_safe('SELECT doltgres_test_shared(22, 0, 0)'),
	'1', 'a reclaim frees a record no process reached since it took it');
is(step($node, 9, $idle, 0),
	'0', 'and the location of a reclaimed record answers nothing');
$looker->quit;

# A process's second handle of a record counts once: its first close leaves
# the record held, and its last frees a condemned slot.
my $twice = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
$holder = $node->background_psql('postgres');
$holder->query_safe(
	"SELECT doltgres_test_shared(9, $twice, 0), doltgres_test_shared(9, $twice, 0)"
);
is(step($node, 33, $twice, 0),
	'1', "a process's two handles of a record count as one holder");
$owner->query_safe("SELECT doltgres_test_shared(17, $twice, 0)");
$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
$before = free_slots($node);
$holder->query_safe('SELECT doltgres_test_shared(10, 0, 0)');
is(free_slots($node), $before,
	"a process's first close of a record it opened twice leaves it held");
$holder->query_safe('SELECT doltgres_test_shared(10, 0, 0)');
is(free_slots($node), $before + 1,
	"a process's last close frees the condemned slot");
$holder->quit;

# A terminated backend's handles are given up at its exit: a record it held
# open is closed, and a record it claimed is published again.
my $held = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
$holder = $node->background_psql('postgres', on_error_stop => 0);
$holder->query_safe("SELECT doltgres_test_shared(9, $held, 0)");
$pid = $holder->query_safe('SELECT pg_backend_pid()');
$owner->query_safe("SELECT doltgres_test_shared(17, $held, 0)");
$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
$before = free_slots($node);
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
ok( $node->poll_query_until(
		'postgres', "SELECT doltgres_test_shared(16, 0, 0) = $before + 1"),
	'a holder terminated by pg_terminate_backend is closed at its exit');
$holder->{run}->finish;

my $claimed = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
($code, $out, $err) =
  $node->psql('postgres', "SELECT doltgres_test_shared(18, $claimed, 0)");
like($err, qr/the test failed after its claim/,
	'a step fails after its claim');
is(step($node, 9, $claimed, 0),
	'1', 'a claim an error dropped leaves the record published');
my $claimer = $node->background_psql('postgres', on_error_stop => 0);
is($claimer->query_safe("SELECT doltgres_test_shared(17, $claimed, 0)"),
	'1', 'a backend claims a published record');
is(step($node, 17, $claimed, 0), '0', 'a second claim answers nothing');
is(step($node, 9, $claimed, 0), '0', 'a claimed record takes no new open');
$pid = $claimer->query_safe('SELECT pg_backend_pid()');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
ok( $node->poll_query_until(
		'postgres', "SELECT doltgres_test_shared(9, $claimed, 0) = 1"),
	"a terminated claimer's record is published again");
$claimer->{run}->finish;

# A make after a free succeeds while another backend sits in a reader begun
# before the free, and the oldest epoch is that reader's until it ends.
hold($node, 'doltgres-shared-reading');
$reader = $node->background_psql('postgres');
start($reader, 'SELECT doltgres_test_shared(11, 0, 0)');
$node->wait_for_event('client backend', 'doltgres-shared-reading');
my $epoch = step($node, 13, 0, 0);
is(step($node, 12, 0, 0),
	$epoch, 'the oldest epoch is the one a paused reader started at');
$before = free_slots($node);
$owner->query_safe("SELECT doltgres_test_shared(17, $claimed, 0)");
$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
is(free_slots($node), $before + 1,
	'a free gives the slot back while a reader begun before it is live');
$owner->query_safe('SELECT doltgres_test_shared(19, 0, 0)');
is(free_slots($node), $before,
	"a make after a free succeeds while another backend's reader began before the free"
);
$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
release($node, 'doltgres-shared-reading');
ok( pump_until(
		$reader->{run}, $reader->{timeout},
		\$reader->{stdout}, qr/^$epoch$/m),
	'the paused reader started at that epoch');
$reader->quit;
ok( $node->poll_query_until(
		'postgres', "SELECT doltgres_test_shared(12, 0, 0) = $epoch + 1"),
	"the oldest epoch is a paused reader's until it ends");

# A FATAL raised under a record's lock, in the last holder of a record freed
# meanwhile: its exit closes the record, which frees the slot, and the lock it
# held is released, so a record made in that slot is locked by its maker.
my $fatal = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
$filler->query_safe(
	'SELECT count(doltgres_test_shared(19, 0, 0)) FROM generate_series(1, (SELECT doltgres_test_shared(16, 0, 0)))'
);
is(free_slots($node), '0', 'the pool is full');
hold($node, 'doltgres-shared-locked');
my $dying = $node->background_psql('postgres', on_error_stop => 0);
start($dying, "SELECT doltgres_test_shared(30, $fatal, 0)");
$node->wait_for_event('client backend', 'doltgres-shared-locked');
$owner->query_safe("SELECT doltgres_test_shared(17, $fatal, 0)");
$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
is(free_slots($node), '0',
	'the record freed under its holder keeps its slot');
release($node, 'doltgres-shared-locked');
ok( $node->poll_query_until(
		'postgres', 'SELECT doltgres_test_shared(16, 0, 0) = 1'),
	"a FATAL under a record's lock frees the condemned slot at the backend's exit"
);
$dying->{run}->finish;
my $made = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
step($node, 1, $made, 9);
is(step($node, 2, $made, 0),
	'9',
	"a FATAL under a record's lock frees a condemned slot, and the new record's lock is taken"
);
$filler->quit;

# A slot freed at its last generation is spent: no make takes it, and its old
# location answers nothing.
$before = free_slots($node);
my $last = $owner->query_safe('SELECT doltgres_test_shared(28, 0, 0)');
is(free_slots($node), $before - 1,
	'a record is made in a slot at its last generation');
is(step($node, 6, $last, 0),
	'0', 'a record at its last generation is opened');
$owner->query_safe("SELECT doltgres_test_shared(17, $last, 0)");
$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
is(free_slots($node), $before - 1,
	'a slot freed at its last generation is spent');
my $left = $before - 1;
$filler = $node->background_psql('postgres', on_error_stop => 0);
is( $filler->query_safe(
		"SELECT count(doltgres_test_shared(19, 0, 0)) FROM generate_series(1, $left)"
	),
	$left,
	'the pool takes a make for each slot but the spent one');
($code, $out, $err) =
  $node->psql('postgres', 'SELECT doltgres_test_shared(19, 0, 0)');
like($err, qr/every test record is in use/, 'no make takes a spent slot');
is(step($node, 9, $last, 0),
	'0', "a spent slot's old location answers nothing");
$filler->quit;
$owner->quit;

# A second server with more connections: the pool's count follows the
# processes. Its pool is the makes' to race for: with the pool full and its
# reclaim switched on, a make frees an idle record and takes its slot; a make
# whose reclaim is held after it took a record, while a backend opens and
# closes that record, goes around and gets a slot; and two makes that race for
# two idle records each get one.
my $wide = PostgreSQL::Test::Cluster->new('wide');
$wide->init;
$wide->append_conf('postgresql.conf', 'max_connections = 200');
$wide->start;
$wide->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$wide->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
my $more = step($wide, 31, 0, 0);
my $connections = $node->safe_psql('postgres', 'SHOW max_connections');
ok( $more > $count
	  && abs(($more - $count) - (200 - $connections) / 8) <= 1,
	"the pool's count follows the server's processes");
is( $wide->safe_psql(
		'postgres',
		"SELECT size = doltgres_test_shared(32, 0, 0) FROM pg_shmem_allocations WHERE name = 'versioned shared memory'"
	),
	't',
	'a server of more processes lists the region at the size its kinds take');

$owner = $wide->background_psql('postgres', on_error_stop => 0);
$filler = $wide->background_psql('postgres', on_error_stop => 0);
my $first = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
$rest = $more - 1;
$filler->query_safe(
	"SELECT count(doltgres_test_shared(19, 0, 0)) FROM generate_series(1, $rest)"
);
step($wide, 24, 0, 1);
is(free_slots($wide), '0', 'the second pool is full');
$filler->query_safe('SELECT doltgres_test_shared(19, 0, 0)');
is(step($wide, 9, $first, 0),
	'0',
	'a full pool whose reclaim frees an idle record makes with no error');

step($wide, 24, 0, 0);
$filler->query_safe(
	'SELECT doltgres_test_shared(14, 0, 0), doltgres_test_shared(14, 0, 0)');
$first = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
my $second = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
step($wide, 24, 0, 1);
is(free_slots($wide), '0', 'the second pool is full with two idle records');
hold($wide, 'doltgres-shared-gated');
my $gate = $wide->background_psql('postgres');
start($gate, 'SELECT doltgres_test_shared(29, 0, 0)');
$wide->wait_for_event('client backend', 'doltgres-shared-gated');
my $racer = $wide->background_psql('postgres', on_error_stop => 0);
start($racer, 'SELECT doltgres_test_shared(19, 0, 0)');
ok( $wide->poll_query_until(
		'postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'versioned flag'"
	),
	'a make waits inside its reclaim, which took the first idle record');
my $other = $wide->background_psql('postgres', on_error_stop => 0);
start($other, 'SELECT doltgres_test_shared(19, 0, 0)');
ok( $wide->poll_query_until(
		'postgres',
		"SELECT count(*) = 2 FROM pg_stat_activity WHERE wait_event = 'versioned flag'"
	),
	'a second make waits inside its reclaim, which took the second idle record'
);
is(step($wide, 6, $first, 0),
	'0', 'a backend opens and closes the record the first reclaim took');
release($wide, 'doltgres-shared-gated');
ok( pump_until(
		$racer->{run}, $racer->{timeout},
		\$racer->{stdout}, qr/^0$/m),
	'a make whose reclaim was held while a backend opened the record goes around and gets a slot'
);
ok( pump_until(
		$other->{run}, $other->{timeout},
		\$other->{stdout}, qr/^0$/m),
	'two makes racing for two idle records each get one');
is( $wide->safe_psql(
		'postgres',
		"SELECT doltgres_test_shared(16, 0, 0), doltgres_test_shared(9, $first, 0), doltgres_test_shared(9, $second, 0)"
	),
	'0|0|0',
	'both idle records went, and the pool is full');
$gate->quit;
$racer->quit;
$other->quit;
$filler->quit;
$owner->quit;

ok( slurp_file($node->logfile) !~ /PANIC|terminated by signal/
	  && slurp_file($wide->logfile) !~ /PANIC|terminated by signal/,
	'no backend ended in a PANIC');

done_testing();
