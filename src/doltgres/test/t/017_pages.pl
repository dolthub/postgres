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

# The contract of the delta's pages (design item 14's S6) in the server,
# through the test module's steps over pages that a test record's lock owns
# and its atomic names, with injection points holding a backend where a case
# needs it: the setting and the two regions; bytes appended and words stored
# by one backend are read by another; an append needs the lock that owns its
# page and room for its record; a name that names no page is refused; a page
# filled while its allocator owns it is read after its link; an allocation
# that fails, and a raise between an allocation and its link, leave nothing
# counted; a page retired under a hold is freed only after the hold ends, by
# an exit too; a second free pass frees nothing a first one claimed; an open
# refuses a slot freed and used again between its loads; writers wait at the
# budget and finish once pages are retired and freed; a slot freed at its
# last generation is spent; and a keyless table's inserts, folded at the
# smallest budget, all take their pages.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('pages');
$node->init;
$node->append_conf('postgresql.conf',
	"autovacuum = off\nlog_error_verbosity = verbose");
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
is( $node->safe_psql(
		'postgres',
		"SELECT boot_val, unit, context FROM pg_settings WHERE name = 'delta_memory'"
	),
	'32768|kB|postmaster',
	'delta_memory is a memory setting of 32MB by default, set at server start'
);
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

# A page of 1 KiB, its bytes and eight words, as a step takes its size: it
# takes a 1 KiB slot of a block, 64 to a block, so the directory's 1,024
# slots and the region's 16 blocks run out together (bugs row 150). A page of
# 8 KiB the same way, and one of a block, 64 KiB, with one word.
my $PAGE = (8 << 32) | (1024 - 64);
my $EIGHT = (8 << 32) | (8192 - 64);
my $LARGE = (1 << 32) | ((64 << 10) - 8);

# The bytes a block holds, which a page of a block alone counts.
my $BLOCK = 64 << 10;

# Runs a statement in a session of its own and returns what it printed,
# which is nothing when it raised or didn't end, so that the case which
# reads it fails by its name.
sub printed
{
	my ($sql) = @_;
	my ($code, $out) = $node->psql(
		'postgres', $sql,
		timeout => $PostgreSQL::Test::Utils::timeout_default,
		timed_out => \my $late);
	return $out // '';
}

# Runs one step over the delta's pages in a session of its own.
sub delta
{
	my ($step, $a, $b) = @_;
	return printed("SELECT doltgres_test_delta($step, $a, $b)");
}

# Allocates a number of pages of a size at once and reads, in the same
# statement, whether the delta gave them, the bytes in use, and the free
# slots.
sub allocated
{
	my ($size, $count) = @_;
	return printed(
		"SELECT doltgres_test_delta(10, $size, $count) || '|' || doltgres_test_delta(0, 0, 0) || '|' || doltgres_test_delta(17, 0, 0)"
	);
}

# Two thousand pages of 8 KiB, a version page and a tuple id page for each of
# a thousand storages, fit the default delta_memory in one allocation.
is(delta(10, $EIGHT, 2000),
	'1', 'two thousand pages of 8 KiB fit the default delta_memory');

# The server's processes decide how many test records there are, one for
# each eight, and the cases below make about two dozen.
$node->append_conf('postgresql.conf',
	"delta_memory = 1MB\nmax_connections = 300");
$node->restart;

# Runs one step over the delta's pages in a session of its own and returns
# the error it raised, with its SQLSTATE. A step that doesn't end is given
# up on, so that its case fails by its name.
sub refused
{
	my ($step, $a, $b) = @_;
	my ($code, $out, $err) = $node->psql(
		'postgres',
		"\\set VERBOSITY verbose\nSELECT doltgres_test_delta($step, $a, $b)",
		timeout => $PostgreSQL::Test::Utils::timeout_default,
		timed_out => \my $late);
	return $err;
}

# Runs one step that raises and then, in the same session, reads the bytes
# in use and the free slots, so that what the raise left is seen before the
# backend's exit gives anything back; returns the error and the two numbers.
sub raised_then
{
	my ($step, $a, $b) = @_;
	my ($code, $out, $err) = $node->psql(
		'postgres',
		"\\set VERBOSITY verbose\nSELECT doltgres_test_delta($step, $a, $b);\n"
		  . "SELECT doltgres_test_delta(0, 0, 0) || '|' || doltgres_test_delta(17, 0, 0);",
		on_error_stop => 0);
	return ($err, $out);
}

# Makes a test record, whose lock owns a page and whose atomic names it.
sub record
{
	return $node->safe_psql('postgres',
		'SELECT doltgres_test_shared(0, 0, 0)');
}

# Makes a test record with a page of a size linked under it.
sub paged
{
	my ($size) = @_;
	my $record = record();
	delta(1, $record, $size);
	return $record;
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

# Starts a statement in a session, whose backend it notes, and returns once
# the session has taken it.
sub start
{
	my ($session, $sql) = @_;
	$session->{pid} = $session->query_safe('SELECT pg_backend_pid()');
	$session->query_until(qr/start/, "\\echo start\n$sql;\n");
	return;
}

# Waits for what a started statement prints on a stream and reports whether
# it came. A statement that never prints it has its backend terminated and
# its session's timer stopped, so that the case which reads it fails by its
# name and the file goes on.
sub awaited
{
	my ($session, $stream, $until) = @_;
	return 1
	  if eval {
		pump_until($session->{run}, $session->{timeout}, $stream, $until);
	  };
	$node->safe_psql('postgres',
		"SELECT pg_terminate_backend($session->{pid})");
	$session->{timeout}->reset;
	return 0;
}

# Ends a session whatever its timer says: a case that waited out another
# session's failure may have outlasted this one's timer, which would end the
# file before the case is reported.
sub leave
{
	my ($session) = @_;
	$session->{timeout}->reset;
	$session->quit;
	return;
}

# Returns what a started statement printed once it ends, or nothing when it
# never ends.
sub ended
{
	my ($session) = @_;
	awaited($session, \$session->{stdout}, qr/^-?\d+$/m);
	my ($printed) = $session->{stdout} =~ /^(-?\d+)$/m;
	return $printed;
}

# Runs one step with the free pass's start reporting each time a backend
# reaches it, and returns what the step printed and how many passes it ran.
sub passes
{
	my ($step, $a, $b) = @_;
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('doltgres-delta-passing', 'notice')");
	my ($code, $out, $err) =
	  $node->psql('postgres', "SELECT doltgres_test_delta($step, $a, $b)");
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-delta-passing')");
	my $count = () = $err =~ /doltgres-delta-passing/g;
	return ($out, $count);
}

# Retires a record's page under one paused reader's hold, starts a second
# reader, whose hold begins after the retirement, runs a retire of another
# record's name, the same page's again or a stale name of its slot, and lets
# both readers go on. Returns what the first read and the second's error: a
# retire of a name that isn't live leaves the page's stamp, so the second is
# still refused the page. A free pass then frees the page.
sub restamped
{
	my ($record, $again) = @_;
	hold('doltgres-test-delta-held');
	my $first = $node->background_psql('postgres', on_error_stop => 0);
	start($first, "SELECT doltgres_test_delta(5, $record, 0)");
	$node->wait_for_event('client backend', 'doltgres-test-delta-held');
	delta(4, $record, 0);
	my $second = $node->background_psql('postgres', on_error_stop => 0);
	start($second,
		"\\set VERBOSITY verbose\nSELECT doltgres_test_delta(5, $record, 0)");
	$node->poll_query_until('postgres',
		"SELECT count(*) = 2 FROM pg_stat_activity WHERE wait_event = 'doltgres-test-delta-held'"
	) or die 'the second reader never paused';
	delta(4, $again, 0);
	release('doltgres-test-delta-held');
	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('doltgres-test-delta-held')");
	my $read = ended($first);
	awaited($second, \$second->{stderr},
		qr/ERROR:  XX000: a name of the delta names no page its hold reaches/
	);
	my $refusal = $second->{stderr};
	leave($first);
	leave($second);
	delta(23, 0, 0);
	return ($read, $refusal);
}

# The region is listed at its blocks, the setting's sixteen and one more for
# the copies compactions take past the budget (bugs rows 110 and 116), and the
# directory at its header, ten cache lines, a cache line for each 1 KiB page
# the region's blocks can hold, that block's included, three words for each
# block, and a word of the free hint for each 64 slots below the block past
# the budget's (bugs row 150).
is( $node->safe_psql(
		'postgres',
		"SELECT name, size FROM pg_shmem_allocations WHERE name LIKE 'versioned delta%' ORDER BY name"
	),
	"versioned delta|1114112\nversioned delta directory|70808",
	"the delta's region and its directory are listed with their sizes");

# On the fresh server the region gives sixteen pages of a block at once, the
# budget exactly; seventeen are past it. A refusal takes nothing, in the
# backend that asked. With the budget full, a compaction's copy past it takes
# the region's one more block, and once that copy is given back a second
# copy takes the block again.
is(allocated($LARGE, 16),
	'1|0|1088', 'an allocation the budget holds is given');
is(allocated($LARGE, 17),
	'0|0|1088', 'an allocation past the budget takes nothing');
is( printed(
		"SELECT doltgres_test_delta(59, $LARGE, 16) || '|' || doltgres_test_delta(0, 0, 0) || '|' || doltgres_test_delta(17, 0, 0)"
	),
	'11|0|1088',
	"a copy past the budget takes the region's one more block, and gives it back for the next copy"
);

# More writers than the delta holds pages for, each needing two pages of a
# block at once: eight take theirs and two wait at the budget; as a fold
# retires what the eight linked, the two take theirs; all finish, and once
# their pages are retired too nothing stays counted.
my @records = map { record() } 1 .. 10;
my @writers =
  map { $node->background_psql('postgres', on_error_stop => 0) } 1 .. 10;
my @early = (0 .. 7);
my @late = (8, 9);

# The writers start once the delta gives sixteen of the pages at once, so that the
# pages earlier cases retired are freed, and the eight it has pages for run
# one at a time before the other two start, so that each ends before the next
# asks. Each wait starts its session's timer, so a slow build fails no case
# by the time it took to reach it.
$node->poll_query_until('postgres',
	"SELECT doltgres_test_delta(10, $LARGE, 16) = 1")
  or die 'the delta never freed the pages earlier cases retired';
my @took;
for my $writer (@early)
{
	$writers[$writer]{timeout}->start;
	start($writers[$writer],
		"SELECT doltgres_test_delta(25, $records[$writer], $LARGE)");
	push @took, ended($writers[$writer]) // 'none';
}
for my $writer (@late)
{
	$writers[$writer]{timeout}->start;
	start($writers[$writer],
		"SELECT doltgres_test_delta(25, $records[$writer], $LARGE)");
}
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 2 FROM pg_stat_activity WHERE wait_event = 'versioned delta budget'"
	),
	'writers the delta has no pages for wait at the budget');
is(join(',', @took),
	'0,0,0,0,0,0,0,0',
	'writers the delta has pages for take them without a wait');
delta(21, $records[$_], 0)   for @early;
$writers[$_]{timeout}->start for @late;
is( join(',',
		map { (ended($writers[$_]) // 0) >= 1 ? 'waited' : 'none' } @late),
	'waited,waited',
	'writers that waited at the budget finish as a fold retires what the others linked'
);
delta(21, $records[$_], 0) for @late;
leave($_) for @writers;
is(delta(0, 0, 0) . '|' . delta(17, 0, 0),
	'0|1088', 'nothing stays counted after the writers and their fold');

my $first = paged($PAGE);
is(delta(2, $first, 77) . '|' . delta(2, $first, 78),
	'0|8', 'an append returns the offset of its record');
is(delta(3, $first, 0) . '|' . delta(3, $first, 8),
	'77|78', 'bytes appended by one backend are read by another');
is(delta(11, $first, 0), '77078', 'one read crosses two appended records');
like(
	refused(3, $first, 9),
	qr/ERROR:  XX000: a read of a page of the delta ends past its filled mark/,
	"a read past a page's filled mark is refused");

# A page's words are stored and loaded across backends, and of two exchanges
# from one value one wins.
delta(6, $first, 5);
is(delta(7, $first, 0),
	'5', 'a word stored by one backend is loaded by another');
is(delta(8, $first, 3) . '|' . delta(7, $first, 0),
	'5|8', 'an add to a word returns the value before');
is( delta(9, $first, (9 << 32) | 8) . '|'
	  . delta(9, $first, (10 << 32) | 8) . '|'
	  . delta(7, $first, 0),
	'8|9|9',
	'of two exchanges of a word from one value one wins');
like(
	refused(7, $first, 8),
	qr/ERROR:  XX000: a word of a page of the delta is past the page's words/,
	"a word past a page's words is refused");

# An append under another record's lock is refused, as is one to a sealed
# page, and a record that doesn't fit changes nothing.
my $other = record();
like(
	refused(12, $first, $other),
	qr/ERROR:  XX000: a page of the delta takes an append or a seal only under the lock that owns it/,
	"an append under another record's lock is refused");
my $small = paged(12);
is(delta(2, $small, 1), '0', 'a record that fits its page is appended');
like(
	refused(2, $small, 2),
	qr/ERROR:  XX000: a record doesn't fit the page it is appended to/,
	"a record that doesn't fit its page is refused");
is(delta(3, $small, 0), '1', "a refused append leaves the page's bytes");
delta(13, $first, 0);
like(
	refused(2, $first, 79),
	qr/ERROR:  XX000: a page of the delta takes an append or a seal only under the lock that owns it/,
	'an append to a sealed page is refused');

# A name past the directory and a name at another generation are refused.
my $name =
  $node->safe_psql('postgres', "SELECT doltgres_test_shared(6, $first, 0)");
like(
	refused(27, 0, 200 << 36),
	qr/ERROR:  XX000: a name of the delta names no page its hold reaches/,
	'a name past the directory is refused');
like(
	refused(27, 0, $name + 1),
	qr/ERROR:  XX000: a name of the delta names no page its hold reaches/,
	"a name at a generation that isn't its slot's is refused");

# A page filled while its allocator owns it is read after its link.
my $filled = record();
cmp_ok(delta(14, $filled, 41),
	'>=', 0, "an owned page's name is the name its link stores");
is(delta(3, $filled, 0),
	'41', 'bytes appended while a page is owned are read after its link');
like(
	refused(16, 12, 5),
	qr/ERROR:  XX000: a record doesn't fit the page it is appended to/,
	"an append past an owned page's capacity is refused");

# A raise between an allocation and its link leaves nothing counted and no
# slot taken, in the backend that raised.
my $used = delta(0, 0, 0);
my $free = delta(17, 0, 0);
my ($error, $left) = raised_then(15, $PAGE, 0);
like(
	$error,
	qr/ERROR:  XX000: the test failed after its allocation/,
	'a step fails after its allocation');
is($left, "$used|$free",
	'a backend that raises between its allocation and its link leaves nothing counted'
);
# The budget's wait refuses a backend that has a hold and one that owns a
# page.
like(
	refused(18, 0, 0),
	qr/ERROR:  XX000: a backend that has a hold on the delta's pages waited at the delta's budget/,
	'the wait at the budget refuses a backend that has a hold');
like(
	refused(18, 1, 0),
	qr/ERROR:  XX000: a backend that owns a page of the delta waited at the delta's budget/,
	'the wait at the budget refuses a backend that owns a page');

# A page retired under a hold begun before its retirement stays counted and
# readable to that hold; a hold begun after can't open it; and the pass that
# follows the hold's end frees it, its block with it.
my $kept = paged($LARGE);
delta(2, $kept, 55);
my $old = printed("SELECT doltgres_test_shared(6, $kept, 0)");
hold('doltgres-test-delta-held');
my $reader = $node->background_psql('postgres', on_error_stop => 0);
start($reader, "SELECT doltgres_test_delta(5, $kept, 0)");
$node->wait_for_event('client backend', 'doltgres-test-delta-held');
$used = delta(0, 0, 0);
delta(4, $kept, 0);
is(delta(0, 0, 0),
	$used, 'a page retired under a hold begun before it stays counted');
delta(4, $kept, 0);
is(delta(0, 0, 0), $used, 'a second retire of one name does nothing');
like(
	refused(3, $kept, 0),
	qr/ERROR:  XX000: a name of the delta names no page its hold reaches/,
	"a hold begun after a page's retirement can't open it");
release('doltgres-test-delta-held');
is(ended($reader), '55',
	'a hold begun before a retirement reads the page until it ends');
leave($reader);
delta(23, 0, 0);
is( delta(0, 0, 0),
	$used - $BLOCK,
	"a page retired under another backend's hold is freed only after that hold ends"
);
like(
	refused(27, 0, $old),
	qr/ERROR:  XX000: a name of the delta names no page its hold reaches/,
	"a freed page's name is refused");

# A second retire of a retired page's name leaves its stamp: a hold begun
# between the two retires is still refused the page.
my $twice = paged($PAGE);
delta(2, $twice, 91);
my ($read, $refusal) = restamped($twice, $twice);
is($read, '91',
	'a hold begun before a retirement reads the page through a second retire of its name'
);
like(
	$refusal,
	qr/ERROR:  XX000: a name of the delta names no page its hold reaches/,
	"a second retire of a retired page's name leaves its stamp");

# A backend terminated while it has a hold keeps no page from being freed.
$kept = paged($LARGE);
hold('doltgres-test-delta-held');
$reader = $node->background_psql('postgres', on_error_stop => 0);
start($reader, "SELECT doltgres_test_delta(5, $kept, 0)");
my $pid = $reader->{pid};
$node->wait_for_event('client backend', 'doltgres-test-delta-held');
$used = delta(0, 0, 0);
delta(4, $kept, 0);
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
$node->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid")
  or die 'the terminated backend never exited';
$reader->{run}->finish;
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-test-delta-held')");
my $epoch = delta(19, 0, 0);
my ($done, $ran) = passes(23, 0, 0);
is( delta(0, 0, 0),
	$used - $BLOCK,
	"a terminated backend's hold keeps no page from being freed");
is("$ran|" . delta(19, 0, 0),
	"1|$epoch", 'a retire of no names runs the free pass and nothing else');

# A hold begun while a retirement is paused between its stamp and its
# advance of the epoch waits for the advance, so it begins after the
# retirement: it is refused the page, and reads no freed memory.
my $stamped = paged($PAGE);
hold('doltgres-delta-stamped');
my $retirer = $node->background_psql('postgres', on_error_stop => 0);
start($retirer, "SELECT doltgres_test_delta(4, $stamped, 0)");
$node->wait_for_event('client backend', 'doltgres-delta-stamped');
$reader = $node->background_psql('postgres', on_error_stop => 0);
start($reader,
	"\\set VERBOSITY verbose\nSELECT doltgres_test_delta(3, $stamped, 0)");
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE pid = $reader->{pid} AND wait_event_type = 'LWLock'"
	),
	'a hold begun inside a retirement waits for its advance of the epoch');
release('doltgres-delta-stamped');
is(ended($retirer), '0', 'a retirement paused after its stamp goes on');
awaited($reader, \$reader->{stderr},
	qr/ERROR:  XX000: a name of the delta names no page its hold reaches/);
like(
	$reader->{stderr},
	qr/ERROR:  XX000: a name of the delta names no page its hold reaches/,
	'a hold begun inside a retirement is refused the page');
leave($retirer);
leave($reader);

# A backend terminated while it owns a page gives the page back at its exit.
$used = delta(0, 0, 0);
$free = delta(17, 0, 0);
hold('doltgres-test-delta-owned');
my $owner = $node->background_psql('postgres', on_error_stop => 0);
start($owner, "SELECT doltgres_test_delta(15, $LARGE, 0)");
$pid = $owner->{pid};
$node->wait_for_event('client backend', 'doltgres-test-delta-owned');
is( delta(0, 0, 0) . '|' . delta(17, 0, 0),
	($used + $BLOCK) . '|' . ($free - 1),
	'a page its allocator owns is counted and has its slot');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
$node->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid")
  or die 'the terminated backend never exited';
$owner->{run}->finish;
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-test-delta-owned')");
is(delta(0, 0, 0) . '|' . delta(17, 0, 0),
	"$used|$free",
	'a backend terminated while it owns a page gives the page back');

# A free pass paused after its claim of a slot: a second pass frees nothing,
# and the count is right once the first goes on.
$kept = paged($LARGE);
$used = delta(0, 0, 0);
hold('doltgres-delta-claimed');
my $passer = $node->background_psql('postgres', on_error_stop => 0);
start($passer, "SELECT doltgres_test_delta(4, $kept, 0)");
$node->wait_for_event('client backend', 'doltgres-delta-claimed');
delta(23, 0, 0);
is(delta(0, 0, 0),
	$used, "a second free pass at a pass's pause frees nothing");
release('doltgres-delta-claimed');
is(ended($passer), '0', 'a paused free pass goes on when woken');
leave($passer);
is( delta(0, 0, 0),
	$used - $BLOCK,
	'a free pass that was paused frees what it claimed');

# An allocation of one page more than there are free slots takes none: the
# pages it took before the refusal are given back, in the backend that asked.
# With every slot taken but the one only a compaction's copy past the budget
# may take (bugs row 110), an allocation is refused, and a writer that needs
# two pages waits at the budget. A free pass that claimed a slot and paused
# doesn't end the wait; once it goes on and frees the retired pages, the
# writer takes its pages and finishes.
my ($had, $open) = (delta(0, 0, 0), delta(17, 0, 0));
is(allocated($PAGE, $open + 1),
	"0|$had|$open",
	'an allocation refused at its last page gives back the pages it took');
my $full = record();
my $linked = delta(20, $full, $PAGE);
is( delta(17, 0, 0),
	'64',
	"pages are linked until only the slots the block past the budget keeps for compactions' copies are free"
);
is(delta(10, $PAGE, 1),
	'0', 'an allocation is refused when no other slot is free');
my $writes = record();
my $writer = $node->background_psql('postgres', on_error_stop => 0);
start($writer, "SELECT doltgres_test_delta(25, $writes, $PAGE)");
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'versioned delta budget'"
	),
	'a writer refused its pages waits at the budget');
hold('doltgres-delta-claimed');
my $folder = $node->background_psql('postgres', on_error_stop => 0);
start($folder, "SELECT doltgres_test_delta(21, $full, 0)");
$node->wait_for_event('client backend', 'doltgres-delta-claimed');
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_stat_activity WHERE wait_event = 'versioned delta budget'"
	),
	'1',
	'a writer still waits while a free pass is paused after its claim');
release('doltgres-delta-claimed');
is(ended($folder), $linked, 'a fold retires every page it linked');
cmp_ok(ended($writer) // -1, '>=', 1,
	"a refusal taken while another backend's free pass is paused ends when that pass ends"
);
leave($folder);
leave($writer);
is(delta(17, 0, 0),
	$open - 2,
	'a writer that waited at the budget links its pages once they fit');

# An open paused between its loads of a slot's word, while the page is freed
# and its slot used again, refuses. The page is retired under an older hold,
# so that it is still there when the open begins; with every other slot
# taken, the last by a replacement's copy past the budget, the page allocated
# after its free takes its slot. That page is
# retired in its turn, so the stamp the open loads is one its hold reaches
# and only its second load of the slot's word refuses it.
my $reused = record();
my $reused_name = delta(1, $reused, $PAGE);
my $refilled = delta(20, $full, $PAGE);
delta(60, $full, $PAGE);
$refilled++;
is( delta(17, 0, 0),
	'63',
	"every slot an ordinary page may take is taken again, and a replacement's copy past the budget takes one the block past the budget keeps"
);
hold('doltgres-test-delta-held');
$reader = $node->background_psql('postgres', on_error_stop => 0);
start($reader, "SELECT doltgres_test_delta(5, $reused, 0)");
$pid = $reader->{pid};
$node->wait_for_event('client backend', 'doltgres-test-delta-held');
delta(4, $reused, 0);
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-test-delta-held')");
hold('doltgres-delta-opening');
my $opener = $node->background_psql('postgres', on_error_stop => 0);
start($opener,
	"\\set VERBOSITY verbose\nSELECT doltgres_test_delta(3, $reused, 0)");
$node->wait_for_event('client backend', 'doltgres-delta-opening');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
$node->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid")
  or die 'the terminated backend never exited';
$reader->{run}->finish;
delta(23, 0, 0);
is(delta(17, 0, 0), '64', "the retired page's slot is free");

my $again = record();
is( delta(1, $again, $PAGE) >> 33,
	$reused_name >> 33,
	"a new page takes the freed page's slot, the only one below the block past the budget's"
);
delta(4, $again, 0);
release('doltgres-delta-opening');
awaited($opener, \$opener->{stderr},
	qr/ERROR:  XX000: a name of the delta names no page its hold reaches/);
like(
	$opener->{stderr},
	qr/ERROR:  XX000: a name of the delta names no page its hold reaches/,
	'an open refuses a slot freed and used again between its loads');
leave($opener);

# A retire of a freed page's name after its slot is used again does nothing:
# with every other slot taken, a page retired and freed leaves its slot to
# the next page, which a second retire of the first page's name leaves live.
delta(23, 0, 0);
my $gone = paged($PAGE);
delta(4, $gone, 0);
my $next = paged($PAGE);
delta(4, $gone, 0);
is( delta(7, $next, 0),
	'0',
	"a retire of a freed page's name after its slot is used again does nothing"
);
delta(2, $next, 92);
($read, $refusal) = restamped($next, $gone);
is($read, '92',
	"a hold begun before a retirement reads the page through a retire of its slot's stale name"
);
like(
	$refusal,
	qr/ERROR:  XX000: a name of the delta names no page its hold reaches/,
	"a retire of a slot's stale name leaves the stamp of the page that took the slot"
);

# A free made between a writer's refusal and its wait ends the wait at once.
# The free is a fold's retire of every page it linked, which runs one free
# pass and advances the epoch once for each sixty-four names.
my $more = record();
hold('doltgres-test-delta-refused');
my $latecomer = $node->background_psql('postgres', on_error_stop => 0);
start($latecomer, "SELECT doltgres_test_delta(25, $more, $PAGE)");
$node->wait_for_event('client backend', 'doltgres-test-delta-refused');
$epoch = delta(19, 0, 0);
($done, $ran) = passes(21, $full, 0);
is($done, $refilled, 'a fold retires every page of its chain');
is( ($ran . '|' . (delta(19, 0, 0) - $epoch)),
	'1|' . int(($refilled + 63) / 64),
	'a retire of many names runs one free pass and advances the epoch once for each sixty-four'
);
release('doltgres-test-delta-refused');
is(ended($latecomer), '1',
	'a free made between a refusal and its wait ends the wait at once');
leave($latecomer);

# An iterator that yields an error after three names: the three are retired
# and the error is returned.
my $few = record();
delta(29, $few, 3);
$free = delta(17, 0, 0);
like(
	refused(30, $few, 0),
	qr/ERROR:  XX000: the test's iterator failed/,
	"an iterator's error is returned by the retire it ends");
is(delta(17, 0, 0),
	$free + 3, 'the names an iterator yielded before its error are retired');

# After mixed sizes are allocated and freed, a page of a block allocates.
delta(21, $writes, 0);
delta(21, $more, 0);
is( delta(26, 0, 0),
	'1',
	'after mixed sizes are allocated and freed, a page of a block allocates');

# A slot freed at its last generation is spent: its name is refused, and no
# allocation takes it again.
delta(22, 0, 0);
$free = delta(17, 0, 0);
my $last = record();
my $spent = delta(1, $last, $PAGE);
delta(4, $last, 0);
is(delta(17, 0, 0),
	$free - 1, 'a slot freed at its last generation is not free again');
like(
	refused(27, 0, $spent),
	qr/ERROR:  XX000: a name of the delta names no page its hold reaches/,
	"a spent slot's last name is refused");
like(
	refused(27, 0, $spent - 1),
	qr/ERROR:  XX000: a name of the delta names no page its hold reaches/,
	'another name of a spent slot is refused');
my $counts = delta(0, 0, 0) . '|' . delta(17, 0, 0);
delta(4, $last, 0);
is(delta(0, 0, 0) . '|' . delta(17, 0, 0),
	$counts, "a retire of a spent slot's name does nothing");

# With every slot of the directory spent, no free would end a wait, so an
# allocation raises 53200 with a hint to restart (bugs row 150).
delta(31, 0, 0);
like(
	refused(10, $PAGE, 1),
	qr/ERROR:  53200: out of shared memory\n.*HINT:  The delta's page directory has spent every slot; restart the server\./s,
	'an allocation from a directory of spent slots raises');

# A restart gives the delta a directory of free slots again.
$node->restart;

# A single record's lock owns a page as a test record's does: it appends to
# the page and seals it. The locks of the two single records and of a test
# record are each refused for one another's pages.
my $OWNS =
  qr/ERROR:  XX000: a page of the delta takes an append or a seal only under the lock that owns it/;
my $tables = record();
delta(32, $tables, 0);
my $spares = record();
delta(32, $spares, 1);
my $tested = paged($PAGE);
is(delta(33, $tables, 0),
	'0', "a single record's lock appends to the page it owns");
is(delta(33, $spares, 1),
	'0', "the second single record's lock appends to the page it owns");
like(refused(33, $tables, 1),
	$OWNS, "a single record's page refuses another single record's lock");
like(refused(33, $spares, 0),
	$OWNS, "the second single record's page refuses the first's lock");
like(refused(33, $tables, 2),
	$OWNS, "a single record's page refuses a test record's lock");
like(refused(33, $tested, 0),
	$OWNS, "a test record's page refuses a single record's lock");
like(refused(33, $tested, 1),
	$OWNS, "a test record's page refuses the second single record's lock");
like(refused(34, $tables, 1),
	$OWNS, "another single record's lock can't seal a single record's page");
is(delta(34, $tables, 0), '0', "a single record's lock seals its page");
like(refused(33, $tables, 0),
	$OWNS, "a sealed page refuses its single record's lock");

# The second single record has a run of a word for each 1 KiB of
# delta_memory: a page linked into one of its words is reached through it, its
# last word is stored by one backend and loaded by another, and the index past
# it is refused, at two sizes of delta_memory.
my $RUN = qr/ERROR:  XX000: a word of a single record's run is past the run/;
my $run_page = delta(35, 3, 41);
is(delta(37, 3, 0),
	$run_page,
	"a page's link stores its name in a word of a single record's run");
is(delta(27, 0, delta(37, 3, 0)),
	'41', "a page named by a word of a run is read back through it");
delta(36, 1023, 99);
is( delta(37, 1023, 0),
	'99',
	"the last word of a run of 1MB of delta_memory is loaded by another backend"
);
like(refused(37, 1024, 0),
	$RUN, "the index past a run of 1MB of delta_memory is refused");
$node->append_conf('postgresql.conf', 'delta_memory = 2MB');
$node->restart;
delta(36, 2047, 77);
is( delta(37, 2047, 0),
	'77',
	"the last word of a run of 2MB of delta_memory is loaded by another backend"
);
like(refused(36, 2048, 1),
	$RUN, "the index past a run of 2MB of delta_memory is refused");

# The delta's records, freed by the epochs as its pages are: the first outer
# and inner records after a restart share a location, so the inner record's
# page refuses the outer record's lock; after a link a hold reaches both
# records and the page; and a backend that raises or is terminated before its
# link gives back both records and their pages.
my $NO_RECORD =
  qr/ERROR:  XX000: a location of the delta names no record its hold reaches/;
my $outer = delta(42, 0, 0);
is(delta(43, $outer, 3),
	'1',
	'the first outer and inner records after a restart share a location');
like(
	refused(43, $outer, 1),
	$OWNS,
	"an inner record's page refuses the lock of the outer record at its location"
);
like(
	refused(43, $outer, 2),
	$OWNS,
	"an inner record's page can't be sealed under another shape's lock at its location"
);
is( delta(43, $outer, 0),
	'8',
	"after a link a hold reaches both records and the page, and the inner record's lock appends to it"
);

# Runs a step that raises and then, in the same session, reads the two pools'
# free records and the bytes in use, before the backend's exit gives anything
# back; returns the error and the three numbers.
sub raised_records
{
	my ($step, $a, $b) = @_;
	my ($code, $out, $err) = $node->psql(
		'postgres',
		"\\set VERBOSITY verbose\nSELECT doltgres_test_delta($step, $a, $b);\n"
		  . "SELECT doltgres_test_delta(44, 0, 0) || '|' || doltgres_test_delta(44, 1, 0) || '|' || doltgres_test_delta(0, 0, 0);",
		on_error_stop => 0);
	return ($err, $out);
}

my $pools = delta(44, 0, 0) . '|' . delta(44, 1, 0) . '|' . delta(0, 0, 0);
my ($fault, $pools_after) = raised_records(42, 5, 1);
like(
	$fault,
	qr/ERROR:  XX000: the test failed before its link/,
	'a backend raises after it built two records and their pages');
is($pools_after, $pools,
	'a backend that raises before its link gives back both records and their pages at once'
);
hold('doltgres-test-delta-made');
my $builder = $node->background_psql('postgres', on_error_stop => 0);
start($builder, "SELECT doltgres_test_delta(42, 5, 2)");
$node->wait_for_event('client backend', 'doltgres-test-delta-made');
isnt(delta(44, 0, 0) . '|' . delta(44, 1, 0) . '|' . delta(0, 0, 0),
	$pools, 'a backend paused before its link holds its records and pages');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($builder->{pid})");
ok( $node->poll_query_until(
		'postgres',
		"SELECT doltgres_test_delta(44, 0, 0) || '|' || doltgres_test_delta(44, 1, 0) || '|' || doltgres_test_delta(0, 0, 0) = '$pools'"
	),
	'a backend terminated before its link gives back both records and their pages'
);
$builder->{run}->finish;
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-test-delta-made')");

# A record opened under a hold begun before its retirement is reached until
# the hold ends; a hold begun after is refused, as are a forged location and
# a freed record's.
my $reached = delta(38, 1, 41);
hold('doltgres-test-delta-held');
my $early = $node->background_psql('postgres', on_error_stop => 0);
start($early, "SELECT doltgres_test_delta(39, $reached, 1)");
$node->wait_for_event('client backend', 'doltgres-test-delta-held');
delta(40, $reached, 0);
like(refused(39, $reached, 0),
	$NO_RECORD, "a hold begun after a record's retirement is refused it");
release('doltgres-test-delta-held');
is(ended($early), '41',
	"a hold begun before a record's retirement reaches it and its page");
leave($early);
like(refused(39, 256 << 36, 0),
	$NO_RECORD, "a location past the pool's records is refused");
like(refused(39, $reached + 1, 0),
	$NO_RECORD, "a location at another generation is refused");
delta(23, 0, 0);
like(refused(39, $reached, 0),
	$NO_RECORD, "a freed record's location is refused");

# With the pool full a make is refused and its writer waits at the budget,
# and it makes its record once a retired one is freed.
delta(51, 0, 0);
my $made = delta(45, 1, 0);
is( delta(52, 1, $made),
	'0',
	'records made in a slot that an unlinked record left read their atomics zeroed'
);
is(delta(38, 0, 0), '-1', 'a make with the pool full is refused');
hold('doltgres-test-delta-refused');
my $waiter = $node->background_psql('postgres', on_error_stop => 0);
start($waiter, "SELECT doltgres_test_delta(46, 1, 0)");
$node->wait_for_event('client backend', 'doltgres-test-delta-refused');
release('doltgres-test-delta-refused');
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE pid = $waiter->{pid} AND wait_event = 'versioned delta budget'"
	),
	'a writer refused a record waits at the budget');
delta(41, 1, 1);
is(ended($waiter), '1',
	'a writer that waited for a record makes it once the pass frees one');
leave($waiter);

# A record's open held between its first load of the record's word and its
# load of its stamp, as case 6 holds a page's: with every other record taken,
# the record is retired, freed, made again in its slot, and retired again at
# the pause, so only the open's second load of the word refuses it.
my $reopened = delta(37, 2, 0);
hold('doltgres-test-delta-held');
my $keeper = $node->background_psql('postgres', on_error_stop => 0);
start($keeper, "SELECT doltgres_test_delta(39, $reopened, 1)");
my $kept_pid = $keeper->{pid};
$node->wait_for_event('client backend', 'doltgres-test-delta-held');
delta(41, 2, 1);
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-test-delta-held')");
hold('doltgres-delta-opening');
my $record_opener = $node->background_psql('postgres', on_error_stop => 0);
start($record_opener,
	"\\set VERBOSITY verbose\nSELECT doltgres_test_delta(39, $reopened, 0)");
$node->wait_for_event('client backend', 'doltgres-delta-opening');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($kept_pid)");
$node->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $kept_pid")
  or die 'the terminated backend never exited';
$keeper->{run}->finish;
delta(23, 0, 0);
is(delta(44, 1, 0), '1', "the retired record's slot is free");
delta(38, 2, 0);
is(delta(44, 1, 0), '0', "a new record takes the freed record's slot");
delta(41, 2, 1);
release('doltgres-delta-opening');
awaited($record_opener, \$record_opener->{stderr}, $NO_RECORD);
like($record_opener->{stderr},
	$NO_RECORD,
	"a record's open refuses a slot freed and made again between its loads");
leave($record_opener);
delta(41, 1, $made);
like(
	refused(50, 0, 0),
	qr/ERROR:  XX000: a backend that owns an unlinked record of the delta waited at the delta's budget/,
	'the wait at the budget refuses a backend that owns an unlinked record');
is(delta(44, 1, 0), 2047, "every record the pool's fill made is free again");

# A hold that keeps a retired record and its page: a pass held between its
# walk of the records and its walk of the pages while the hold ends frees
# neither, and the next pass frees both.
my $walked = delta(38, 2, 52);
my $slots = delta(17, 0, 0);
hold('doltgres-test-delta-held');
$early = $node->background_psql('postgres', on_error_stop => 0);
start($early, "SELECT doltgres_test_delta(39, $walked, 1)");
$node->wait_for_event('client backend', 'doltgres-test-delta-held');
delta(40, $walked, 0);
hold('doltgres-delta-walked');
my $walker = $node->background_psql('postgres', on_error_stop => 0);
start($walker, "SELECT doltgres_test_delta(23, 0, 0)");
$node->wait_for_event('client backend', 'doltgres-delta-walked');
release('doltgres-test-delta-held');
is(ended($early), '52', 'a hold begun before a retirement reads the record');
leave($early);
release('doltgres-delta-walked');
is(ended($walker), '0', 'a pass held after its walk of the records goes on');
leave($walker);
is( delta(17, 0, 0) . '|' . delta(44, 1, 0),
	"$slots|2046",
	'a pass that walked the records under a hold frees neither the record nor its page after the hold ends'
);
delta(23, 0, 0);
is( delta(17, 0, 0) . '|' . delta(44, 1, 0),
	($slots + 1) . '|2047',
	'the next pass frees the record and its page');

# A record retired in one call with two pages: one advance of the epoch, a
# hold begun before reaches the record and its page, and one begun after
# reaches neither; with a hundred pages, two advances. An iterator that fails
# after the record's page retires both and returns its error, and a cancel
# pending at a retire is raised after it, with the record retired.
my $batched = delta(38, 3, 71);
hold('doltgres-test-delta-held');
$early = $node->background_psql('postgres', on_error_stop => 0);
start($early, "SELECT doltgres_test_delta(39, $batched, 1)");
$node->wait_for_event('client backend', 'doltgres-test-delta-held');
my $epoch_at = delta(19, 0, 0);
delta(48, $batched, 1);
is( delta(19, 0, 0),
	$epoch_at + 1,
	'a record retired in one call with two pages advances the epoch once');
like(refused(39, $batched, 0),
	$NO_RECORD, 'a hold begun after the call is refused the record');
release('doltgres-test-delta-held');
is(ended($early), '71',
	'a hold begun before the call reaches the record and its page');
leave($early);
my $many = delta(38, 4, 0);
$epoch_at = delta(19, 0, 0);
delta(48, $many, 99);
is( delta(19, 0, 0),
	$epoch_at + 2,
	'a record retired in one call with a hundred pages advances the epoch twice'
);
my $failing = delta(38, 5, 0);
like(
	refused(49, $failing, 0),
	qr/ERROR:  XX000: the test's iterator failed/,
	"a retire whose iterator fails returns the iterator's error");
like(refused(39, $failing, 0),
	$NO_RECORD, 'a retire whose iterator failed retired the record');
my $cancelled = delta(38, 6, 0);
hold('doltgres-delta-stamped');
my $canceller = $node->background_psql('postgres', on_error_stop => 0);
start($canceller,
	"\\set VERBOSITY verbose\nSELECT doltgres_test_delta(40, $cancelled, 0)");
$node->wait_for_event('client backend', 'doltgres-delta-stamped');
$node->safe_psql('postgres', "SELECT pg_cancel_backend($canceller->{pid})");
release('doltgres-delta-stamped');
awaited($canceller, \$canceller->{stderr}, qr/ERROR:  57014/);
like(
	$canceller->{stderr},
	qr/ERROR:  57014: canceling statement due to user request/,
	'a cancel pending at a retire is raised after it');
leave($canceller);
like(refused(39, $cancelled, 0),
	$NO_RECORD, 'a retire that a cancel was pending at retired its record');

# A retirement stores a page's stamp and then its retired state, and a
# record's in the same order: a free pass that loaded its oldest epoch before
# the retirement and walks while the retirement is held between the two
# stores frees nothing, so a hold begun before the retirement reads the page,
# and opens the record, after both (bugs row 50).
my $stamp_page = paged($PAGE);
delta(2, $stamp_page, 63);
hold('doltgres-test-delta-held');
my $page_reader = $node->background_psql('postgres', on_error_stop => 0);
start($page_reader, "SELECT doltgres_test_delta(5, $stamp_page, 0)");
$node->wait_for_event('client backend', 'doltgres-test-delta-held');
hold('doltgres-delta-oldest');
my $page_passer = $node->background_psql('postgres', on_error_stop => 0);
start($page_passer, "SELECT doltgres_test_delta(23, 0, 0)");
$node->wait_for_event('client backend', 'doltgres-delta-oldest');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-delta-oldest')");
hold('doltgres-delta-stamping');
my $page_retirer = $node->background_psql('postgres', on_error_stop => 0);
start($page_retirer, "SELECT doltgres_test_delta(4, $stamp_page, 0)");
$node->wait_for_event('client backend', 'doltgres-delta-stamping');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-delta-stamping')");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-delta-oldest')");
is(ended($page_passer), '0',
	'a pass that loaded its oldest epoch before a retirement walks while the retirement is held'
);
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-delta-stamping')");
is(ended($page_retirer), '0',
	'a retirement held between its stamp and its state goes on');
release('doltgres-test-delta-held');
is(ended($page_reader), '63',
	"a hold begun before a page's retirement reads it after a pass walked between the retirement's stores"
);
leave($_) for ($page_reader, $page_passer, $page_retirer);

my $stamp_inner = delta(38, 9, 64);
hold('doltgres-test-delta-held');
my $inner_reader = $node->background_psql('postgres', on_error_stop => 0);
start($inner_reader, "SELECT doltgres_test_delta(39, $stamp_inner, 1)");
$node->wait_for_event('client backend', 'doltgres-test-delta-held');
hold('doltgres-delta-oldest');
my $inner_passer = $node->background_psql('postgres', on_error_stop => 0);
start($inner_passer, "SELECT doltgres_test_delta(23, 0, 0)");
$node->wait_for_event('client backend', 'doltgres-delta-oldest');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-delta-oldest')");
hold('doltgres-delta-stamping');
my $inner_retirer = $node->background_psql('postgres', on_error_stop => 0);
start($inner_retirer, "SELECT doltgres_test_delta(41, 9, 1)");
$node->wait_for_event('client backend', 'doltgres-delta-stamping');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-delta-stamping')");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-delta-oldest')");
is(ended($inner_passer), '0',
	"a pass that loaded its oldest epoch before a record's retirement walks while it is held"
);
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-delta-stamping')");
is(ended($inner_retirer), '0',
	"a record's retirement held between its stamp and its state goes on");
release('doltgres-test-delta-held');
is(ended($inner_reader), '64',
	"a hold begun before a record's retirement opens it after a pass walked between the retirement's stores"
);
leave($_) for ($inner_reader, $inner_passer, $inner_retirer);

# A record's list (the delta's lists): its seventh entry, on its first list
# page, and its 519th, past a root that grew, are read by another backend; a
# reader's hold finds every old entry after the root grew; an index past the
# length is refused; and a closed list takes no store.
my $listed = delta(42, 10, 0);
is(delta(53, $listed, 7), '7', 'a list takes seven entries');
is( delta(54, $listed, 6),
	'1006',
	"a list's seventh entry, on its first list page, is read by another backend"
);
delta(53, $listed, 518);
hold('doltgres-test-delta-held');
my $list_reader = $node->background_psql('postgres', on_error_stop => 0);
start($list_reader, "SELECT doltgres_test_delta(55, $listed, 0)");
$node->wait_for_event('client backend', 'doltgres-test-delta-held');
is(delta(53, $listed, 519),
	'519', "a list's root grows past its first list page");
release('doltgres-test-delta-held');
is(ended($list_reader), '518',
	"a reader's hold finds every old entry after the list's root grew");
leave($list_reader);
is(delta(54, $listed, 518),
	'1518', "the entry past a new root is read by another backend");
like(
	refused(54, $listed, 519),
	qr/ERROR:  XX000: an index of a list is past its length/,
	"an index past a list's length is refused");
like(
	refused(56, $listed, 0),
	qr/ERROR:  XX000: a closed list takes no change/,
	'a closed list takes no store');
delta(57, $listed, 0);

# A record freed at its last generation is spent: its locations are refused,
# a retire of it does nothing, and a pool of spent slots raises.
delta(23, 0, 0);
my $free_inner = delta(44, 1, 0);
delta(47, 0, 0);
my $final = delta(38, 7, 0);
delta(40, $final, 0);
is( delta(44, 1, 0),
	$free_inner - 1,
	'a record freed at its last generation is not free again');
like(refused(39, $final, 0),
	$NO_RECORD, "a spent record's last location is refused");
like(refused(39, $final - 1, 0),
	$NO_RECORD, "another location of a spent record is refused");
delta(41, 7, 1);
is( delta(44, 1, 0),
	$free_inner - 1,
	'a retire of a spent record does nothing');
delta(41, 0, 1);
delta(23, 0, 0);
delta(47, 0, 1);
like(
	refused(38, 8, 0),
	qr/ERROR:  XX000: every slot of the delta's inner test records is spent; restart the server/,
	'a make from a pool of spent slots raises');

ok(slurp_file($node->logfile) !~ /PANIC|terminated by signal/,
	'no backend ended in a PANIC');

# The seed of the delta's shapes is one for every backend, and the restart
# after a crash draws it anew.
my $seed = delta(58, 0, 0);
ok( $seed =~ /^-?[1-9]\d*$/ && delta(58, 0, 0) eq $seed,
	"two backends read one seed of the delta's shapes");
$node->append_conf('postgresql.conf', 'restart_after_crash = on');
$node->reload;
$node->poll_query_until('postgres',
	"SELECT current_setting('restart_after_crash') = 'on'")
  or die 'the postmaster never took restart_after_crash';
my $crashed = -s $node->logfile;
$node->psql('postgres', 'SELECT doltgres_test_escalate()');
$node->wait_for_log(qr/database system is ready to accept connections/,
	$crashed);
isnt(delta(58, 0, 0),
	$seed, "the restart after a crash draws the delta's seed anew");

# Bugs row 116's cause: pages of nine sizes under 8 KiB, each allocated and
# dropped, leave nothing held, so a page of a block for each of the budget's
# blocks, 64 free slots each with the block past the budget's, then
# allocate at once. The DSA area kept an
# empty superblock of 64 KiB for each of the sizes, and refused them though
# the budget held them.
delta(10, $_, 1) for (1560, 1816, 2048, 2616, 3120, 4096, 5456, 6552, 8192);
my $empty = delta(17, 0, 0);
is( allocated($LARGE, $empty / 64 - 1),
	"1|0|$empty",
	'pages of nine sizes under 8 KiB, allocated and dropped, leave the whole budget to pages of a block'
);

# Bugs row 116, TxnPF 10's sequence: at delta_memory = 1MB, a keyless table's
# inserts of 1 KB rows, folded every 150 rows, each take their pages. Each
# block holds pages of one size, and a block whose last page is freed goes
# back to be taken by any size, so a writer under the budget always gets its
# page; the DSA area's kept empty superblocks left the insert of row 583
# waiting at the budget forever.
$node->safe_psql('postgres', 'CREATE TABLE kilobytes (k int4, v text)');
my $inserted = 0;
for my $n (1 .. 900)
{
	my ($code, $out) = $node->psql(
		'postgres',
		"SELECT doltgres_test_write('kilobytes', true, 'INSERT INTO kilobytes VALUES ($n, repeat(''x'', 1000))')",
		timeout => $PostgreSQL::Test::Utils::timeout_default,
		timed_out => \my $late);
	last if $late || $code != 0;
	$inserted = $n;
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('kilobytes')")
	  if $n % 150 == 0;
}
is($inserted, 900,
	"a keyless table's 1 KB inserts, folded every 150 rows, all take their pages at 1MB of delta_memory"
);

# Bugs row 105 (PLAN item 5's relfilenodes and their files): a relfilenode
# Postgres unlinks takes the fork's storage with it, at the commit of a drop or
# of a swap of the relation's relfilenode (TRUNCATE, VACUUM FULL), at the abort
# of the transaction that made it, and in a standby's redo of either, so its
# delta's pages go back to the budget: no fold takes the versions of a storage
# that is gone, and a delta full of them would leave every writer waiting. An
# aborted drop keeps the storage. Folds stay off, so the versions are unfolded,
# and timed checkpoints stay off, since one gives an aborted transaction's
# pages back too.
my $unlinking = PostgreSQL::Test::Cluster->new('unlinking');
$unlinking->init(allows_streaming => 1);
$unlinking->append_conf('postgresql.conf',
	"delta_memory = 8MB\nfold_threshold = '1GB'\nfold_timeout = 3600s\ncheckpoint_timeout = 1h"
);
$unlinking->start;
$unlinking->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
$unlinking->backup('unlinking_backup');
my $unlinked_standby = PostgreSQL::Test::Cluster->new('unlinked_standby');
$unlinked_standby->init_from_backup($unlinking, 'unlinking_backup',
	has_streaming => 1);
$unlinked_standby->start;
my $delta_used = sub {
	return $_[0]->safe_psql('postgres', 'SELECT doltgres_test_delta(0, 0, 0)');
};
my $below = sub {
	my ($on, $bar) = @_;
	return $on->poll_query_until('postgres',
		"SELECT doltgres_test_delta(0, 0, 0) < $bar");
};
$unlinking->wait_for_replay_catchup($unlinked_standby);
my ($primary_start, $standby_start) =
  ($delta_used->($unlinking), $delta_used->($unlinked_standby));
my @tables = map { "unlinked_$_" } 1 .. 10;
for my $table (@tables)
{
	$unlinking->safe_psql(
		'postgres', qq{
CREATE TABLE $table (k int4, v text);
SELECT doltgres_test_write('$table', true, 'INSERT INTO $table SELECT g, repeat(''u'', 1000) FROM generate_series(1, 100) g');
});
}
$unlinking->wait_for_replay_catchup($unlinked_standby);
my ($primary_filled, $standby_filled) =
  ($delta_used->($unlinking), $delta_used->($unlinked_standby));
cmp_ok($primary_filled, '>', $primary_start + 500_000,
	"ten tables' unfolded rows hold the delta's pages");
$unlinking->safe_psql('postgres', "DROP TABLE $_") for @tables[0 .. 3];
$unlinking->safe_psql('postgres', "TRUNCATE $_") for @tables[4 .. 6];
$unlinking->safe_psql('postgres', "VACUUM FULL $_") for @tables[7 .. 9];
ok( $below->(
		$unlinking, $primary_start + int(($primary_filled - $primary_start) / 2)
	),
	"a dropped table's storage gives its delta's pages back at the drop's commit"
);
$unlinking->wait_for_replay_catchup($unlinked_standby);
ok( $below->(
		$unlinked_standby,
		$standby_start + int(($standby_filled - $standby_start) / 2)),
	"a standby's redo of the drops and swaps gives the storages' pages back");

$unlinking->safe_psql(
	'postgres', q{
CREATE TABLE kept_unlinked (k int4, v text);
SELECT doltgres_test_write('kept_unlinked', true, 'INSERT INTO kept_unlinked SELECT g, repeat(''k'', 1000) FROM generate_series(1, 100) g');
});
my $held = $delta_used->($unlinking);
$unlinking->safe_psql('postgres',
	'BEGIN; DROP TABLE kept_unlinked; ROLLBACK');
is( $unlinking->safe_psql(
		'postgres',
		"SELECT doltgres_test_standing('kept_unlinked', 'SELECT count(*) FROM kept_unlinked')"
	),
	'100',
	'an aborted drop keeps the storage and its rows');
cmp_ok($delta_used->($unlinking), '>=', $held - 65536,
	"an aborted drop keeps the storage's pages");

# The bar is half of what the aborted table's rows took, as the drops' is: the
# delta may take a block for another tree meanwhile, the identity tree's for the
# table's pairs among them.
my $before_abort = $delta_used->($unlinking);
my $aborted_filled = (split /\n/, $unlinking->safe_psql(
	'postgres', q{
BEGIN;
CREATE TABLE aborted_unlinked (k int4, v text);
SELECT doltgres_test_write('aborted_unlinked', true, 'INSERT INTO aborted_unlinked SELECT g, repeat(''a'', 1000) FROM generate_series(1, 1000) g');
SELECT doltgres_test_delta(0, 0, 0);
ROLLBACK;
}))[-1];
ok($below->($unlinking, $before_abort + int(($aborted_filled - $before_abort) / 2)),
	"an aborted table's storage gives its delta's pages back at the abort");

done_testing();
