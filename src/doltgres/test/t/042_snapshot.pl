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


# The fork's reads under Postgres' snapshots (PLAN items 2 and 7; step (5)),
# through the test module: a table's base written by doltgres_test_base, rows
# a transaction inserts into its delta by doltgres_test_insert, and scans by
# doltgres_test_scan under the statement's snapshot and doltgres_test_scan_dirty
# under a Dirty one.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('snapshot');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

# A table's base holds rows 1 and 2; a writer inserts row 3 into its delta and
# stays open.
$node->safe_psql(
	'postgres', q{
CREATE TABLE source_base (a int4, b text);
INSERT INTO source_base VALUES (1, 'a'), (2, 'b');
CREATE TABLE source_new (a int4, b text);
INSERT INTO source_new VALUES (3, 'c');
CREATE TABLE t (a int4 PRIMARY KEY, b text);
SELECT doltgres_test_base('t', 'source_base');
});

# The scan's rows' columns, each row's ctid and xmin left off.
my $rows =
  q{SELECT string_agg(substring(r from '^\S+ \S+ (.*)$'), ',') FROM doltgres_test_scan('t', false, -1) r};

my $writer = $node->background_psql('postgres');
$writer->query_safe(
	q{BEGIN; SELECT doltgres_test_insert('t', 'source_new', false, 0)});
my $xid = $writer->query_safe(q{SELECT pg_current_xact_id()});
my $reader = $node->background_psql('postgres');
$reader->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ});

is($writer->query_safe($rows),
	'1 a,2 b,3 c',
	"a transaction's scan gives the base and its own delta rows");
is($reader->query_safe($rows),
	'1 a,2 b', "a scan doesn't give a running transaction's delta rows");

# A Dirty scan gives the running writer's row, with its xid stored in the
# snapshot as its xmin, and stores none for the base's committed rows.
is( $node->safe_psql(
		'postgres',
		q{SELECT string_agg(regexp_replace(r, '^\S+ \S+ (\S+) \S+ (\S+ \S+)$', '\1 \2'), ',') FROM doltgres_test_scan_dirty('t') r}
	),
	"1 0 0,2 0 0,3 $xid 0",
	"a Dirty scan reports the running writer of the row it gives");

$writer->query_safe('COMMIT');
is( $reader->query_safe($rows),
	'1 a,2 b',
	"a snapshot taken while a transaction runs sees it in progress after it commits"
);
is($node->safe_psql('postgres', $rows),
	'1 a,2 b,3 c',
	"a seq scan gives the rows its snapshot sees of the base and the delta");

$reader->quit;
$writer->quit;
# (4b)'s section 8: a NonVacuumable snapshot sees a row a committed delete ended
# while a snapshot taken before the delete is held, as heap's
# HeapTupleSatisfiesNonVacuumable answers HEAPTUPLE_RECENTLY_DEAD, and not once
# the holder ends; an aborted insert's row it never sees.
$node->safe_psql(
	'postgres', q{
CREATE TABLE source_horizon (a int4, b text);
INSERT INTO source_horizon VALUES (1, 'a'), (2, 'b');
CREATE TABLE horizon (a int4 PRIMARY KEY, b text);
SELECT doltgres_test_insert('horizon', 'source_horizon', false, 0);
});
my $kept = q{SELECT string_agg(substring(r from '^\S+ \S+ (.*)$'), ',' ORDER BY r) FROM doltgres_test_scan_nonvacuumable('horizon') r};
my $holder = $node->background_psql('postgres');
$holder->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class});
$node->safe_psql('postgres',
	q{SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT doltgres_test_write('horizon', true, 'DELETE FROM horizon WHERE a = 1')});
my $before = $node->safe_psql('postgres', $kept);
$holder->query_safe('COMMIT');
$holder->quit;
my $after = $node->safe_psql('postgres', $kept);
$node->safe_psql('postgres',
	q{BEGIN; SELECT doltgres_test_write('horizon', true, 'INSERT INTO horizon VALUES (3, ''c'')'); ROLLBACK});
my $aborted = $node->safe_psql('postgres', $kept);
is( "$before;$after;$aborted",
	'1 a,2 b;2 b;2 b',
	'a NonVacuumable snapshot sees a deleted row until its deleter passes the horizon');

$node->stop;

done_testing();
