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

# The contract of host's process slots and fixed arrays (design item 14's S2)
# in the server, through the test module's steps over its slot of two words
# for each process, the notes and the slots of its single record, and the long
# lock of its pooled records, with injection points pausing a backend where a
# case needs it: a process's words are loaded by other backends, the list of
# the processes holds every number that loads, and the words read zero once
# the process has exited or was terminated, the lowest index first; an array's
# entry is set and read under its lock, an index past the array is refused,
# and a record made in a freed slot reads its entries zeroed; a long lock is
# held shared by two backends at once; slots are
# claimed only through the long lock of the record they name, held shared,
# and read through it held exclusively, each claim once; a mark takes the name
# only when no record with a slot is named; and of two marks that race for a
# stale name, the loser empties nothing.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('slots');
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

# Runs one step of the test module in a session of its own and returns the
# error it raised, with its SQLSTATE.
sub refused
{
	my ($step, $location, $value) = @_;
	my ($code, $out, $err) = $node->psql('postgres',
		"\\set VERBOSITY verbose\nSELECT doltgres_test_shared($step, $location, $value)"
	);
	return $err;
}

# Attaches an injection point that waits.
sub hold
{
	my ($point) = @_;
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('$point', 'wait')");
	return;
}

# Wakes one waiter of an injection point.
sub wake
{
	my ($point) = @_;
	$node->safe_psql('postgres', "SELECT injection_points_wakeup('$point')");
	return;
}

# Detaches an injection point, so no later backend waits at it.
sub detach
{
	my ($point) = @_;
	$node->safe_psql('postgres', "SELECT injection_points_detach('$point')");
	return;
}

# Waits until a backend has left the list of activity, which its exit does
# only after the callback that zeroes its words has run.
sub exited
{
	my ($pid) = @_;
	$node->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid")
	  or die "backend $pid never exited";
	return;
}

# Starts a statement in a session and returns once the session has taken it.
sub start
{
	my ($session, $sql) = @_;
	$session->query_until(qr/start/, "\\echo start\n$sql;\n");
	return;
}

# Returns whether a number of backends wait at an injection point and a
# number wait for a record's lock.
sub waiting
{
	my ($point, $paused, $locked) = @_;
	return $node->poll_query_until('postgres',
		"SELECT count(*) FILTER (WHERE wait_event = '$point') = $paused AND count(*) FILTER (WHERE wait_event_type = 'LWLock' AND wait_event = 'versioned shared memory') = $locked FROM pg_stat_activity"
	);
}

# A backend stores its two words; other backends load them by its process
# number, and it loads its own.
my $storer = $node->background_psql('postgres', on_error_stop => 0);
my $proc = $storer->query_safe('SELECT doltgres_test_shared(34, 0, 70)');
my $pid = $storer->query_safe('SELECT pg_backend_pid()');
is(step(35, $proc, 0),
	'70', "a process's word stored by it is loaded by another backend");
is(step(35, $proc, 1),
	'71', "a process's second word is loaded by another backend");
is($storer->query_safe('SELECT doltgres_test_shared(36, 0, 0)'),
	'71', 'a process loads its own word');
my $listed = step(39, $proc, 0);
my $processes = $listed >> 32;
ok( ($listed & 1) == 1 && $processes > 0,
	'the list of the processes holds a backend that stored a word, and every process in it loads'
);
is(step(35, $processes - 1, 0), '0', 'the last process of the list loads');
like(
	refused(35, $processes, 0),
	qr/ERROR:  XX000: a process's number is past the server's processes/,
	"a process number past the server's processes is refused");

# The words read zero once the backend has exited, and for a new backend that
# takes its number.
$storer->quit;
exited($pid);
is(step(35, $proc, 0) . '|' . step(35, $proc, 1),
	'0|0', "a process's words read zero after it exits");
my $connections = $node->safe_psql('postgres', 'SHOW max_connections');
my $took = $node->safe_psql('postgres',
	"\\connect\nSELECT doltgres_test_shared(37, 0, 0), doltgres_test_shared(36, 0, 0);\n"
	  x (2 * $connections));
my @reused = grep { /^\Q$proc\E\|/ } split /\n/, $took;
my @nonzero = grep { $_ ne "$proc|0" } @reused;
ok(@reused > 0 && @nonzero == 0,
	"a new backend that takes an exited process's number reads zero words");

# A terminated backend's words read zero as well.
$storer = $node->background_psql('postgres', on_error_stop => 0);
$proc = $storer->query_safe('SELECT doltgres_test_shared(34, 0, 80)');
is(step(35, $proc, 1), '81', 'a second backend stores its words');
$pid = $storer->query_safe('SELECT pg_backend_pid()');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
exited($pid);
is(step(35, $proc, 0) . '|' . step(35, $proc, 1),
	'0|0', "a terminated process's words read zero");
$storer->{run}->finish;

# A backend's exit is held between the two stores that zero its words: its
# first word reads zero while its second still holds its value. The hold is
# named by the id of the tests' kind of words, so it is this kind's zeroing
# that is held, wherever the server's table lists the kind.
$storer = $node->background_psql('postgres', on_error_stop => 0);
$proc = $storer->query_safe('SELECT doltgres_test_shared(34, 0, 90)');
$pid = $storer->query_safe('SELECT pg_backend_pid()');
hold('doltgres-shared-zeroing-000000007e570004');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
$node->poll_query_until('postgres',
	"SELECT count(*) FILTER (WHERE wait_event = 'doltgres-shared-zeroing-000000007e570004') = 1 OR count(*) FILTER (WHERE pid = $pid) = 0 FROM pg_stat_activity"
) or die 'the terminated backend neither waited nor exited';
is(step(35, $proc, 0) . '|' . step(35, $proc, 1),
	'0|91', "an exit zeroes a process's words lowest index first");
detach('doltgres-shared-zeroing-000000007e570004');
wake('doltgres-shared-zeroing-000000007e570004');
exited($pid);
is( step(35, $proc, 0) . '|' . step(35, $proc, 1),
	'0|0',
	"an exit held between its stores zeroes the rest of the words once it goes on"
);
$storer->{run}->finish;

# A note set by one backend under the table's lock is read by another, and an
# index past the notes is refused.
step(38, 2, 77);
is(step(40, 2, 0), '77', 'an entry set by one backend is read by another');
is(step(40, 0, 0), '0', 'an entry never set reads zero');
like(
	refused(38, 4, 5),
	qr/ERROR:  XX000: a shared array's entry is past its count/,
	"a set of an entry past an array's count is refused");
like(
	refused(40, 4, 0),
	qr/ERROR:  XX000: a shared array's entry is past its count/,
	"a read of an entry past an array's count is refused");
is( step(40, 2, 0),
	'77',
	"an entry past an array's count is refused, and the entry reads as before"
);

# A pooled record's note is set and read. With every other slot of the pool
# taken, the record is freed and the next make takes its slot, where the note
# reads zero.
my $maker = $node->background_psql('postgres', on_error_stop => 0);
my $noted = $maker->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
step(48, $noted, 55);
is(step(49, $noted, 0), '55', "a pooled record's entry is set and read");
my $free = step(16, 0, 0);
$maker->query_safe(
	"SELECT count(doltgres_test_shared(19, 0, 0)) FROM generate_series(1, $free)"
);
$maker->query_safe("SELECT doltgres_test_shared(17, $noted, 0)");
$maker->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
is(step(16, 0, 0), '1',
	"the freed record's slot is the pool's one free slot");
my $x = $maker->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
is(step(49, $x, 0),
	'0', 'a record made in a freed slot reads its entries zeroed');
$maker->quit;
ok( $node->poll_query_until(
		'postgres', "SELECT doltgres_test_shared(16, 0, 0) = $free"),
	"the maker's unpublished records are free again");

# Each other record is made and published by one owner session, which claims
# and frees the ones a case needs gone. The first of the cases' records is the
# one made in the freed slot, whose location is of the slot's second
# generation.
my $owner = $node->background_psql('postgres', on_error_stop => 0);
my $y = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
my $z = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');

# Two backends hold a record's long lock shared, each paused at an injection
# point; a third that asks for it exclusively waits until both let go.
hold('doltgres-shared-barred');
my $first = $node->background_psql('postgres');
start($first, "SELECT doltgres_test_shared(41, $x, 0)");
my $second = $node->background_psql('postgres');
start($second, "SELECT doltgres_test_shared(41, $x, 0)");
ok( waiting('doltgres-shared-barred', 2, 0),
	'two backends hold a long lock shared at once');
my $taker = $node->background_psql('postgres');
start($taker, "SELECT doltgres_test_shared(42, $x, 0)");
ok( waiting('doltgres-shared-barred', 2, 1),
	'a backend that asks for a long lock exclusively waits for its shared holders'
);
detach('doltgres-shared-barred');
wake('doltgres-shared-barred');
ok( $node->poll_query_until('postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE state = 'idle' AND query LIKE '%doltgres_test_shared(41,%'"
	  )
	  && waiting('doltgres-shared-barred', 1, 1),
	'the exclusive take still waits while one backend holds the long lock shared'
);
wake('doltgres-shared-barred');
ok( pump_until($taker->{run}, $taker->{timeout}, \$taker->{stdout}, qr/^0$/m),
	'the exclusive take gets the long lock once both shared holders let go');
$first->quit;
$second->quit;
$taker->quit;

# With no record marked a claim records nothing. With one marked, two
# backends claim through its long lock, which a third holds shared meanwhile,
# until every slot is claimed; a claim through another record's long lock
# records nothing; and a reader holding the marked record's long lock
# exclusively reads each claim once.
is(step(47, 0, 0), '-1', 'the slots name no record at first');
is(step(44, $x, 1024), '0', 'a claim with no record marked answers false');
is(step(43, $x, 0), '1', 'a mark over no name marks its record');
hold('doltgres-shared-barred');
my $sharer = $node->background_psql('postgres');
start($sharer, "SELECT doltgres_test_shared(41, $x, 0)");
ok(waiting('doltgres-shared-barred', 1, 0),
	"a backend holds the marked record's long lock shared");
my @claimers =
  ($node->background_psql('postgres'), $node->background_psql('postgres'));
is( step(44, $y, 2048),
	'0',
	"a claim through another record's long lock during a marking answers false"
);
my $answers = '';

for my $claim (0 .. 9)
{
	my $value = 1 << $claim;
	$answers .= $claimers[ $claim % 2 ]
	  ->query_safe("SELECT doltgres_test_shared(44, $x, $value)");
}
is($answers, '1111111100', "claims past an array's count answer false");
detach('doltgres-shared-barred');
wake('doltgres-shared-barred');
$sharer->quit;
$_->quit for @claimers;
my $full = (8 << 32) + 255;
is( step(45, $x, 0),
	$full,
	'a reader holding the barrier exclusively reads every claimed entry once and nothing else'
);

# A read and an unmark through a record the slots don't name are refused, and
# a second mark answers false, with the name and the claims as they were.
like(
	refused(45, $y, 0),
	qr/ERROR:  XX000: a slots array doesn't name the record of the barrier it is reached through/,
	'a read through a record that is not the one named is refused');
like(
	refused(46, $y, 0),
	qr/ERROR:  XX000: a slots array doesn't name the record of the barrier it is reached through/,
	'an unmark through a record that is not the one named is refused');
is( $node->safe_psql(
		'postgres',
		"SELECT doltgres_test_shared(43, $y, 0), doltgres_test_shared(47, 0, 0)"
	),
	"0|$x",
	'a second mark while a live record is named answers false and names that record'
);
is(step(45, $x, 0),
	$full,
	'a refused read, a refused unmark, and a second mark change nothing');

# After an unmark under its barrier, another record's mark succeeds and reads
# no slot.
step(46, $x, 0);
is(step(47, 0, 0), '-1', 'an unmark clears the name');
is( $node->safe_psql(
		'postgres',
		"SELECT doltgres_test_shared(43, $y, 0), doltgres_test_shared(45, $y, 0), doltgres_test_shared(47, 0, 0)"
	),
	"1|0|$y",
	"after an unmark under its barrier another record's mark succeeds and reads no slot"
);

# A record freed while it is named leaves a stale name, which the next mark
# takes, emptying the slots.
is(step(44, $y, 1) . step(44, $y, 2),
	'11', 'claims are made through the second marked record');
$owner->query_safe("SELECT doltgres_test_shared(17, $y, 0)");
$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
is(step(47, 0, 0), $y, 'a freed record leaves its name in the slots');
my $unused = step(16, 0, 0);
my $refill = $node->background_psql('postgres', on_error_stop => 0);
$refill->query_safe(
	"SELECT count(doltgres_test_shared(19, 0, 0)) FROM generate_series(1, $unused)"
);
is(step(16, 0, 0), '0', "the freed record's slot holds another record");
is( step(43, $z, 0),
	'1',
	'a mark over the name of a freed record whose slot holds another record succeeds'
);
is( step(45, $z, 0),
	'0',
	'a mark over the name of a freed record succeeds, and the array reads empty after it'
);
$refill->quit;
ok( $node->poll_query_until(
		'postgres', "SELECT doltgres_test_shared(16, 0, 0) = $unused"),
	"the slots taken for the case are free again");

# A record freed while a backend holds it open keeps its slot, and its name
# counts, until that backend closes it.
my $holder = $node->background_psql('postgres', on_error_stop => 0);
$holder->query_safe("SELECT doltgres_test_shared(9, $z, 0)");
$owner->query_safe("SELECT doltgres_test_shared(17, $z, 0)");
$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
my $w = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
is( step(43, $w, 0),
	'0',
	'a mark over the name of a condemned record a holder still has open answers false'
);
$holder->query_safe('SELECT doltgres_test_shared(10, 0, 0)');
is(step(43, $w, 0),
	'1', "a mark succeeds once the condemned record's last holder closed it");
$holder->quit;

# Two marks over a stale name: the loser loads the name and pauses before its
# swap, the winner marks and its record's claims are made, and the loser then
# answers false and empties nothing.
$owner->query_safe("SELECT doltgres_test_shared(17, $w, 0)");
$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
my $winner = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
my $loser = $owner->query_safe('SELECT doltgres_test_shared(0, 0, 0)');
hold('doltgres-shared-marking');
my $slow = $node->background_psql('postgres');
start($slow, "SELECT doltgres_test_shared(43, $loser, 0)");
$node->wait_for_event('client backend', 'doltgres-shared-marking');
detach('doltgres-shared-marking');
is(step(43, $winner, 0),
	'1', 'the first of two marks over a stale name to swap the name wins');
is(step(44, $winner, 1) . step(44, $winner, 2) . step(44, $winner, 4),
	'111', "the winner's record's claims are made");
wake('doltgres-shared-marking');
ok(pump_until($slow->{run}, $slow->{timeout}, \$slow->{stdout}, qr/^0$/m),
	'the mark that lost the name answers false');
$slow->quit;
is( step(45, $winner, 0),
	(3 << 32) + 7,
	'of two marks over a stale name, the one that loses the name answers false and the winner reads every one of its claims'
);

# A claim pauses between its add and its write; a reader that asks for the
# barrier exclusively waits for it, and then reads the entry it wrote.
hold('doltgres-shared-claiming');
my $claimer = $node->background_psql('postgres');
start($claimer, "SELECT doltgres_test_shared(44, $winner, 8)");
$node->wait_for_event('client backend', 'doltgres-shared-claiming');
my $reader = $node->background_psql('postgres');
start($reader, "SELECT doltgres_test_shared(45, $winner, 0)");
ok( waiting('doltgres-shared-claiming', 1, 1),
	'a reader that asks for the barrier exclusively waits for a claim held between its add and its write'
);
detach('doltgres-shared-claiming');
wake('doltgres-shared-claiming');
my $written = (4 << 32) + 15;
ok( pump_until(
		$reader->{run}, $reader->{timeout},
		\$reader->{stdout}, qr/^$written$/m),
	'a reader waits for a claim held between its add and its write, and then reads the entry written'
);
$claimer->quit;
$reader->quit;
$owner->quit;

# A kept kind's words are left as they are at their process's exit, by a
# return and by a termination, and the next backend that takes the number
# finds them through its own slot and stores over them.
my $keeper = $node->background_psql('postgres', on_error_stop => 0);
my $kept = $keeper->query_safe('SELECT doltgres_test_shared(68, 0, 70)');
is($kept >> 32,
	0, "a kept kind's words are zero for the first process of a number");
$proc = $kept & 0xFFFFFFFF;
$keeper->query_safe('SELECT doltgres_test_shared(34, 0, 70)');
$pid = $keeper->query_safe('SELECT pg_backend_pid()');
$keeper->quit;
exited($pid);
is(step(35, $proc, 0) . '|' . step(35, $proc, 1),
	'0|0',
	"the exited process's words of the kind that isn't kept read zero");
is(step(69, $proc, 0) . '|' . step(69, $proc, 1),
	'70|71',
	"a kept kind's words read the same after their process has exited");
$took = $node->safe_psql('postgres',
	"\\connect\nSELECT doltgres_test_shared(68, 0, 80);\n" x
	  (2 * $connections));
my @heirs = grep { ($_ & 0xFFFFFFFF) == $proc } split /\n/, $took;
is( $heirs[0] >> 32,
	70,
	"a new backend that takes an exited process's number finds its kept words through its own slot"
);
is(step(69, $proc, 0) . '|' . step(69, $proc, 1),
	'80|81',
	"the kept words a number's next owner stored over are what others load");
$keeper = $node->background_psql('postgres', on_error_stop => 0);
$kept = $keeper->query_safe('SELECT doltgres_test_shared(68, 0, 90)');
$proc = $kept & 0xFFFFFFFF;
$pid = $keeper->query_safe('SELECT pg_backend_pid()');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
exited($pid);
$keeper->{run}->finish;
is(step(69, $proc, 0) . '|' . step(69, $proc, 1),
	'90|91',
	"a kept kind's words read the same after their process was terminated");

ok(slurp_file($node->logfile) !~ /PANIC|terminated by signal/,
	'no backend ended in a PANIC');

done_testing();
