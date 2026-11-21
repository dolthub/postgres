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

# The fork's parallel scans over a storage's base (PLAN item 5), through the
# test module: doltgres_test_parallel begins scans of the fork's table access
# method routine on one shared parallel scan and reads them in turn, as a
# parallel query's workers would, and the rows they give together are
# compared with the serial scan's and with the heap source's, as sorted
# multisets.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('parallel');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

# columns returns a scan's rows' columns, each row's ctid and xmin left off,
# sorted.
sub columns
{
	my ($query) = @_;
	return $node->safe_psql('postgres',
		"SELECT c FROM (SELECT substring(r from '^\\S+ \\S+ (.*)\$') AS c FROM $query r) s ORDER BY c"
	);
}

# parallel returns the rows of scans sharing one parallel scan, sorted.
sub parallel
{
	my ($target, $workers, $again, $resave, $min, $max) = @_;
	my ($lo, $hi) = map { defined $_ ? "'$_'" : 'NULL' } ($min, $max);
	return columns(
		"doltgres_test_parallel('$target', $workers, $again, $resave, $lo, $hi)"
	);
}

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_many (a int4, b text);
INSERT INTO source_many SELECT n, 'r' || n FROM generate_series(1, 1000) n;
CREATE TABLE many (a int4 PRIMARY KEY, b text);
SELECT doltgres_test_base('many', 'source_many');
});
my $heap = $node->safe_psql('postgres',
	q{SELECT a || ' ' || b FROM source_many ORDER BY a || ' ' || b});
is(columns(q{doltgres_test_scan('many', false, -1)}),
	$heap, "the serial scan gives the heap source's rows");
foreach my $workers (1, 2, 3, 7)
{
	is(parallel('many', $workers, 'false', 0),
		$heap,
		"scans sharing one pscan give the serial scan's rows, each once");
}

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_keyless (a int4, b text);
INSERT INTO source_keyless SELECT n % 3, 'x' FROM generate_series(1, 30) n;
CREATE TABLE keyless (a int4, b text);
SELECT doltgres_test_base('keyless', 'source_keyless');
});
is( parallel('keyless', 4, 'false', 0),
	$node->safe_psql(
		'postgres',
		q{SELECT a || ' ' || b FROM source_keyless ORDER BY a || ' ' || b}),
	"each copy of a keyless table's equal rows is read once");

$node->safe_psql('postgres', 'CREATE TABLE empty_table (a int4)');
is(parallel('empty_table', 3, 'false', 0), '', 'an empty table gives no row');

is(parallel('many', 3, 'true', 0),
	$heap, 'a reinitialized shared scan read again gives every row again');

is( parallel('many', 3, 'false', 0, '(0,100)', '(2,10)'),
	columns(q{doltgres_test_tidrange('many', '(0,100)', '(2,10)', false)}),
	"a parallel tid range scan gives the serial range's rows");

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_later (a int4, b text);
INSERT INTO source_later VALUES (1, 'later');
});
my $later =
  $node->safe_psql('postgres', q{SELECT 'source_later'::regclass::oid});
is( parallel('many', 3, 'false', $later),
	$heap,
	"a base saved after the shared scan is made leaves every scan reading the leader's base"
);

# Bugs row 106: a fold whose drop passes its point keeps the base it replaced
# in the storage's state while the drop takes the rows that fold applied out of
# the delta. A parallel scan begun after such a fold reads the newest base, and
# one in a session whose snapshot predates a row its table's fold applied reads
# the replaced base, whose versions that fold kept for the snapshot; each gives
# the serial scan's rows, and the second only the rows its snapshot sees.
for my $table ('folded_after', 'folded_before')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (a int4, b text)");
	$node->safe_psql('postgres',
		"SELECT doltgres_test_write('$table', true, 'INSERT INTO $table VALUES (1, ''x''), (2, ''y''), (3, ''z'')')"
	);
}
$node->safe_psql('postgres', "SELECT doltgres_test_fold('folded_after')");
is( parallel('folded_after', 3, 'false', 0),
	columns(q{doltgres_test_scan('folded_after', false, -1)}),
	"a parallel scan begun after a fold whose drop passed its point gives the serial scan's rows"
);
my $before = $node->background_psql('postgres');
$before->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$node->safe_psql('postgres',
	"SELECT doltgres_test_write('folded_before', true, 'INSERT INTO folded_before VALUES (4, ''w'')')"
);
$node->safe_psql('postgres', "SELECT doltgres_test_fold('folded_before')");
my $held_columns = sub {
	return $before->query_safe(
		"SELECT c FROM (SELECT substring(r from '^\\S+ \\S+ (.*)\$') AS c FROM $_[0] r) s ORDER BY c"
	);
};
my $held_parallel = $held_columns->(
	q{doltgres_test_parallel('folded_before', 3, false, 0, NULL, NULL)});
is( $held_parallel,
	$held_columns->(q{doltgres_test_scan('folded_before', false, -1)}),
	"a parallel scan in a session whose snapshot predates the fold gives that session's serial scan's rows"
);
is( $held_parallel,
	"1 x\n2 y\n3 z",
	"that scan gives the rows its snapshot sees, and not the row committed after it"
);
$before->quit;

# Bugs row 309's parallel units: a parallel query's scan of a table whose rows
# are all in the delta, run by the leader and two workers, reads the delta's
# run at most once in each of them, at 2,000 rows and at 5,000, where each
# unit read and sorted the whole run; it gives every row once; and the scan
# rescanned on a nested loop's inner side for each of three outer rows gives
# every row each time.
my $parallel_on = q{
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_material = off;
};
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
});
foreach my $rows (2000, 5000)
{
	my ($code, $out, $err) = $node->psql(
		'postgres', qq{
BEGIN;
$parallel_on
CREATE TABLE units_$rows (a int4, b text) USING heap;
ALTER TABLE units_$rows SET (parallel_workers = 2);
INSERT INTO units_$rows SELECT n, repeat('u', 34) FROM generate_series(1, $rows) n;
ANALYZE units_$rows;
SELECT injection_points_attach('doltgres-scan-run', 'notice');
SELECT count(*) FROM units_$rows;
SELECT injection_points_detach('doltgres-scan-run');
\\echo ONCE
SELECT count(*), count(DISTINCT a), sum(a) FROM units_$rows;
\\echo PLAN
EXPLAIN (COSTS OFF) SELECT count(*) FROM (SELECT n FROM generate_series(1, 3) n WHERE random() >= 0) o, units_$rows u WHERE u.a > 0;
\\echo RESCANNED
SELECT count(*) FROM (SELECT n FROM generate_series(1, 3) n WHERE random() >= 0) o, units_$rows u WHERE u.a > 0;
ROLLBACK;
});
	my $runs = () = $err =~ /doltgres-scan-run/g;
	ok( $runs >= 1 && $runs <= 3,
		"a parallel scan of $rows delta rows reads the delta's run at most once in each process ($runs)"
	);
	is( ($out =~ /ONCE\n(\S+)\n/)[0],
		"$rows|$rows|" . ($rows * ($rows + 1) / 2),
		"a parallel scan of $rows delta rows gives every row once");
	like(
		($out =~ /PLAN\n(.*)RESCANNED/s)[0],
		qr/Nested Loop.*Gather.*Parallel Seq Scan on units_$rows/s,
		"the rescanned plan has the parallel scan of $rows rows on a nested loop's inner side"
	);
	is( ($out =~ /RESCANNED\n(\d+)/)[0],
		3 * $rows,
		"a parallel scan of $rows delta rows rescanned for each outer row gives every row each time"
	);
}

$node->stop;
done_testing();
