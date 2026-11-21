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

# The counts of map versions at their writers' commit status entries (bugs row
# 148): a TOAST relation's value map's versions and a free space map's takes
# count at their writer's entry while they stay in a delta, so no status move
# drops the entry of a writer a later fold still asks about.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('counts');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');

# Each map version a transaction pushes counts at its writer's commit status
# entry, so the entry stays while the version does. A drop as the status
# table's move makes one, of an ended entry nothing counts, then can't
# take the entry of a writer whose values or takes are still in a delta: a
# TOAST relation's fold after its table's fold, and a fold that meets a take
# whose row ended before an earlier fold, both read the writer's commit.
sub dropped_if_uncounted
{
	my ($xid) = @_;
	$node->safe_psql('postgres',
		"SELECT CASE WHEN doltgres_test_status_table(1, $xid, 0) = 0 THEN doltgres_test_status_table(2, $xid, 0) END"
	);
	return $node->safe_psql('postgres', "SELECT doltgres_test_status_table(1, $xid, 0)");
}
$node->safe_psql(
	'postgres', q{
CREATE TABLE source_counted (a int4, b text);
INSERT INTO source_counted VALUES (1, repeat(md5('c'), 300));
CREATE TABLE counted (a int4 PRIMARY KEY, b text);
CREATE TABLE source_spent (a int4);
INSERT INTO source_spent VALUES (1), (2);
CREATE TABLE spent (a int4);
});
my $writer = (split /\n/,
	  $node->safe_psql('postgres',
		q{BEGIN; SELECT doltgres_test_insert('counted', 'source_counted', false, 0); SELECT txid_current(); COMMIT})
)[-1];
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('counted')});
ok(dropped_if_uncounted($writer) > 0,
	'a writer\'s entry still counts its values after its table\'s fold takes its rows\' counts back');
my (undef, $folded) = $node->psql('postgres',
	q{SELECT doltgres_test_fold(reltoastrelid::regclass) FROM pg_class WHERE relname = 'counted'});
is($folded, 't', 'a TOAST relation\'s fold after its table\'s fold reads its values\' writer\'s commit');
my $taker = (split /\n/,
	  $node->safe_psql('postgres',
		q{BEGIN; SELECT doltgres_test_insert('spent', 'source_spent', false, 0); SELECT txid_current(); COMMIT}))[-1];
$node->safe_psql('postgres',
	q{SELECT doltgres_test_write('spent', true, 'DELETE FROM spent WHERE a = 1'); SELECT doltgres_test_fold('spent')});
ok(dropped_if_uncounted($taker) > 0,
	'a writer\'s entry still counts its take of a row ended before the fold');
$node->safe_psql('postgres',
	q{SELECT doltgres_test_insert('spent', 'source_spent', false, 0)});
(undef, $folded) = $node->psql('postgres', q{SELECT doltgres_test_fold('spent')});
is($folded, 't', 'a later fold that meets the take reads its writer\'s commit');


# An abort and a rollback to a savepoint give back the counts of the versions
# their prune takes out, so a rolled-back savepoint leaves its committed
# transaction's count as it found it, and once VACUUM frees their takes and a
# later fold with rows to apply drops them, neither writer's entry counts
# anything.
$node->safe_psql(
	'postgres', q{
CREATE TABLE source_rolled (a int4, b text);
INSERT INTO source_rolled VALUES (1, repeat(md5('r'), 300));
CREATE TABLE rolled (a int4 PRIMARY KEY, b text);
});
my $kept = (split /\n/,
	  $node->safe_psql('postgres',
		q{BEGIN; SAVEPOINT s; SELECT doltgres_test_insert('rolled', 'source_rolled', false, 0); ROLLBACK TO s; SELECT txid_current(); COMMIT})
)[-1];
my $aborted = (split /\n/,
	  $node->safe_psql('postgres',
		q{BEGIN; SELECT doltgres_test_insert('rolled', 'source_rolled', false, 0); SELECT txid_current(); ROLLBACK}))[-1];
$node->safe_psql('postgres',
	q{SELECT doltgres_test_vacuum('rolled', 'on'); SELECT doltgres_test_insert('rolled', 'source_rolled', false, 0)});
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('rolled')});
is( $node->safe_psql('postgres',
		"SELECT doltgres_test_status_table(1, $kept, 0), doltgres_test_status_table(1, $aborted, 0)"),
	'0|0',
	'a rolled-back savepoint and an aborted insert leave their writers\' entries counting nothing once their takes go');


# A row lock counts nothing at its writer's entry, so one rolled back to a
# savepoint leaves the committed transaction's count as its writes left it.
$node->safe_psql(
	'postgres', q{
CREATE TABLE source_locked (a int4, b text);
INSERT INTO source_locked VALUES (1, 'one');
CREATE TABLE source_later (a int4, b text);
INSERT INTO source_later VALUES (2, 'two');
CREATE TABLE locked (a int4 PRIMARY KEY, b text);
});
my $locker = (split /\n/,
	  $node->safe_psql(
		'postgres', q{
BEGIN;
SELECT doltgres_test_insert('locked', 'source_locked', false, 0);
SAVEPOINT s;
SELECT doltgres_test_write('locked', true, 'SELECT a FROM locked FOR UPDATE');
ROLLBACK TO s;
SELECT txid_current();
COMMIT;
}))[-1];
$node->safe_psql('postgres',
	q{SELECT doltgres_test_vacuum('locked', 'on'); SELECT doltgres_test_insert('locked', 'source_later', false, 0)});
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('locked')});
# The locker's first write recorded the table's identity pairs, which count at
# its entry until a checkpoint's save drops them (bugs row 164).
$node->safe_psql('postgres', 'CHECKPOINT');
is($node->safe_psql('postgres', "SELECT doltgres_test_status_table(1, $locker, 0)"),
	'0', 'a row lock rolled back to a savepoint leaves its committed transaction\'s entry counting nothing');


# Every free space map version a write pushes counts at its writer's entry,
# and a fold gives back only what was counted (bugs row 305). An UPDATE of 40
# rows on full pages flags a block for each row it moves off one, 40 flags;
# an INSERT of 20 rows into another table by the same transaction pushes 20
# rows and their 20 takes, 40 versions, as many as the flags. The first
# table's fold gives back the UPDATE's versions, flags included, and leaves
# the entry counting the INSERT's 40, so a drop of an ended entry that counts
# nothing passes it by, and the second table's fold reads the writer's
# commit. With the fold launcher held and no fold worker it started before
# its hold left running, no fold but the case's own folds or drops a version
# while the case counts, and each fold waits until no other process holds an
# xmin or an xid at or below the writer, such as an autovacuum worker's,
# which would keep the fold's horizon below it.
sub horizon_past
{
	my ($xid) = @_;
	$node->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid <> pg_backend_pid() AND (age(backend_xmin) >= age('$xid'::xid) OR age(backend_xid) >= age('$xid'::xid))"
	) or die "a process holds the horizon at or below $xid";
	return;
}
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-fold-launch', 'wait')");
$node->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-launch'"
) or die 'the fold launcher is not held';
$node->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'fold worker'"
) or die 'a fold worker still runs';
my $filler = (
	split /\n/,
	$node->safe_psql(
		'postgres', q{
CREATE TABLE flagged (id int4 PRIMARY KEY, v text);
BEGIN;
INSERT INTO flagged SELECT g, repeat('x', 900) FROM generate_series(1, 400) g;
SELECT txid_current();
COMMIT;
CREATE TABLE inserted (id int4, v text);
}))[-1];
horizon_past($filler);
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('flagged')});
my $flagger = (
	split /\n/,
	$node->safe_psql(
		'postgres', q{
BEGIN;
UPDATE flagged SET v = repeat('y', 900) WHERE id <= 40;
INSERT INTO inserted SELECT g, 'b' FROM generate_series(1, 20) g;
SELECT txid_current();
COMMIT;
}))[-1];
horizon_past($flagger);
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('flagged')});
is(dropped_if_uncounted($flagger), '40',
	'a writer\'s entry counts its other table\'s versions after its first table\'s fold gives back its flags'
);
(undef, $folded) =
  $node->psql('postgres', q{SELECT doltgres_test_fold('inserted')});
is($folded, 't', 'the other table\'s fold reads its writer\'s commit');
is( $node->safe_psql(
		'postgres', "SELECT doltgres_test_status_table(1, $flagger, 0)"),
	'0',
	'the writer\'s entry counts nothing once both tables fold');
is( $node->safe_psql(
		'postgres',
		q{SELECT (SELECT count(*) FROM flagged WHERE v LIKE 'y%'), (SELECT count(*) FROM inserted)}
	),
	'40|20',
	'both tables read the transaction\'s rows');

$node->stop;
done_testing();
