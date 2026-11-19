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

# The contract of host's sweep of a pool (design item 14's S2) in the server,
# through the test module's steps over its pooled records, each with a key:
# a sweep lists each published record with its location and its key, and no
# record that isn't published, of another pool, freed, or spent; a listed
# location that was freed opens nothing, also once its slot holds another
# record, which a new sweep lists with its own key; a sweep paused between its
# first load of a slot's word and its read of the key doesn't list a record
# freed and made again meanwhile, and lists one another backend opened
# meanwhile; a find by key returns the record of its key; and two backends
# that find or make one key end with one record.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('sweep');
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

# Detaches an injection point, so no later backend waits at it, and wakes its
# one waiter when asked to.
sub detach
{
	my ($point) = @_;
	$node->safe_psql('postgres', "SELECT injection_points_detach('$point')");
	return;
}

# Wakes the one waiter of an injection point.
sub wake
{
	my ($point) = @_;
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

# Makes a record with a key in the owner's session, publishes it, and returns
# its location.
my $owner = $node->background_psql('postgres', on_error_stop => 0);

sub published
{
	my ($key) = @_;
	$owner->query_safe("SELECT doltgres_test_shared(53, 0, $key)");
	return $owner->query_safe('SELECT doltgres_test_shared(20, 0, 0)');
}

# Claims a record in the owner's session and frees it.
sub freed
{
	my ($location) = @_;
	$owner->query_safe("SELECT doltgres_test_shared(17, $location, 0)");
	$owner->query_safe('SELECT doltgres_test_shared(14, 0, 0)');
	return;
}

# A published record is listed with its location and its key, and a record
# its maker hasn't published isn't.
is(step(50, -1, -1), '0', 'a sweep of a pool with no record lists nothing');
my $first = published(11);
is(step(50, $first, 11),
	'1', "a published record is listed with its location and its atomic's value");
is(step(50, -1, -1), '1', 'a sweep lists one pair for one published record');
$owner->query_safe('SELECT doltgres_test_shared(53, 0, 21)');
is(step(50, -1, -1),
	'1', 'a record its maker has not published is not listed');
my $second = $owner->query_safe('SELECT doltgres_test_shared(20, 0, 0)');
is(step(50, $second, 21), '1', 'the record is listed once it is published');

# A claimed record isn't listed, and is listed again once its claimant has
# published it again.
$owner->query_safe("SELECT doltgres_test_shared(17, $second, 0)");
is(step(50, $second, -1), '0', 'a claimed record is not listed');
is($owner->query_safe('SELECT doltgres_test_shared(20, 0, 0)'),
	$second, 'a claimed record published again keeps its location');
is(step(50, $second, 21),
	'1', 'a record its claimant published again is listed again');

# A record freed while a backend holds it open isn't listed, nor once that
# backend has closed it.
my $holder = $node->background_psql('postgres', on_error_stop => 0);
$holder->query_safe("SELECT doltgres_test_shared(9, $second, 0)");
is(step(50, $second, 21),
	'1', 'a record a backend holds open is listed');
freed($second);
is(step(50, $second, -1),
	'0', 'a condemned record a holder still has open is not listed');
$holder->query_safe('SELECT doltgres_test_shared(10, 0, 0)');
is(step(50, $second, -1), '0', 'a freed record is not listed');
$holder->quit;

# A record in a slot at its last generation is listed, and the slot isn't once
# the record is freed and the slot is spent.
my $last = $owner->query_safe('SELECT doltgres_test_shared(28, 0, 0)');
is(step(50, $last, 0),
	'1', "a record at its slot's last generation is listed");
freed($last);
is(step(50, $last, -1),
	'0', 'a slot spent at its last generation is not listed');

# Another pool's record isn't listed, and a sweep of that pool lists only its
# own.
my $other = $owner->query_safe('SELECT doltgres_test_shared(54, 0, 31)');
is(step(50, -1, 31), '0', 'a record of another pool is not listed');
is(step(55, $other, 31), '1', 'a sweep of the other pool lists its record');
is(step(55, -1, -1),
	'1', 'a sweep of the other pool lists no record of the first');

# A find by key returns the record of its key among the published ones.
my $third = published(41);
is(step(56, 0, 11), $first, 'a find by key returns the record of its key');
is(step(56, 0, 41), $third, 'a find by another key returns its record');
is(step(56, 0, 99),
	'-1', 'a find by a key no record holds returns nothing');

# With every other slot of the pool taken, a listed record is freed and the
# next make takes its slot: the old location opens nothing, and a new sweep
# lists the new record's location with the new record's key.
my $filler = $node->background_psql('postgres', on_error_stop => 0);
my $free = step(16, 0, 0);
$filler->query_safe(
	"SELECT count(doltgres_test_shared(19, 0, 0)) FROM generate_series(1, $free)"
);
freed($third);
is(step(9, $third, 0), '0', 'a location listed and then freed opens nothing');
my $fourth = published(42);
isnt($fourth, $third, "the freed record's slot holds a record of another location");
is(step(9, $third, 0),
	'0',
	'the old location still opens nothing once its slot holds another record');
is( step(50, $fourth, 42) . step(50, $third, -1) . step(50, -1, 41),
	'100',
	"a new sweep lists the new record's location with the new record's value"
);

# A sweep pauses between its first load of the first published slot's word
# and its read of the key. Meanwhile that record is freed, and its slot, the
# pool's one free slot, is made again with another key and published: the
# sweep lists nothing at the old location.
hold('doltgres-shared-sweeping');
my $sweeper = $node->background_psql('postgres');
start($sweeper, "SELECT doltgres_test_shared(50, $first, -1)");
$node->wait_for_event('client backend', 'doltgres-shared-sweeping');
detach('doltgres-shared-sweeping');
freed($first);
my $fifth = published(51);
isnt($fifth, $first, "the first record's slot holds a record of another location");
wake('doltgres-shared-sweeping');
pump_until($sweeper->{run}, $sweeper->{timeout}, \$sweeper->{stdout},
	qr/^\d+$/m);
like($sweeper->{stdout}, qr/^0$/m,
	'a sweep paused before its read does not list a record freed and made again meanwhile'
);
$sweeper->quit;
is(step(50, $fifth, 51),
	'1', "a later sweep lists the slot's new record with its own value");

# A sweep pauses there again while another backend opens the record and keeps
# it open: the sweep lists the record.
hold('doltgres-shared-sweeping');
$sweeper = $node->background_psql('postgres');
start($sweeper, "SELECT doltgres_test_shared(50, $fifth, 51)");
$node->wait_for_event('client backend', 'doltgres-shared-sweeping');
detach('doltgres-shared-sweeping');
$holder = $node->background_psql('postgres', on_error_stop => 0);
is($holder->query_safe("SELECT doltgres_test_shared(9, $fifth, 0)"),
	'1', 'a backend opens the record a paused sweep is at');
wake('doltgres-shared-sweeping');
pump_until($sweeper->{run}, $sweeper->{timeout}, \$sweeper->{stdout},
	qr/^\d+$/m);
like($sweeper->{stdout}, qr/^1$/m,
	'a sweep paused before its read lists a record another backend opened meanwhile'
);
$sweeper->quit;
$holder->quit;
$filler->quit;
ok( $node->poll_query_until(
		'postgres', "SELECT doltgres_test_shared(16, 0, 0) = $free"),
	"the filler's unpublished records are free again");

# Two backends find or make one key: the first finds none and pauses, the
# second finds none, makes the record, and publishes it; the first then finds
# it under the makers' lock and gives its own up.
hold('doltgres-shared-finding');
my $slow = $node->background_psql('postgres');
start($slow, 'SELECT doltgres_test_shared(57, 0, 61)');
$node->wait_for_event('client backend', 'doltgres-shared-finding');
detach('doltgres-shared-finding');
my $made = step(57, 0, 61);
wake('doltgres-shared-finding');
pump_until($slow->{run}, $slow->{timeout}, \$slow->{stdout}, qr/^\d+$/m);
like($slow->{stdout}, qr/^$made$/m,
	'a backend whose first find was empty finds the record another made meanwhile'
);
$slow->quit;
is( step(50, -1, 61) . '|' . step(56, 0, 61),
	"1|$made",
	'two backends that find or make one key end with one record');
is(step(57, 0, 61), $made, 'a later find or make of the key finds the record');
$owner->quit;

ok(slurp_file($node->logfile) !~ /PANIC|terminated by signal/,
	'no backend ended in a PANIC');

done_testing();
