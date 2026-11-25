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

# The table's part of a fork table's index and bitmap scan costs (design item
# 16; bugs row 312), each term read back through EXPLAIN's costs: an
# index-only scan reads no row of the table; the primary key's own index reads
# its leaves, which the node cache holds here; a secondary index's entry costs
# a descent of the table's tree, a comparison a level above the leaves, and,
# past the node cache, Mackert and Lohman's leaves at random; a keyless
# table's entry descends by the row's hash the same way; and a bitmap scan's
# row costs a tuple id resolution and a descent.

use strict;
use warnings FATAL => 'all';
use POSIX qw(ceil);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('costs');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
$node->safe_psql(
	'postgres', q{
CREATE TABLE costed (id int4 PRIMARY KEY, v int4, pad text);
INSERT INTO costed SELECT g, g, repeat('x', 40) FROM generate_series(1, 20000) g;
CREATE INDEX costed_v ON costed (v);
CREATE TABLE keyless (v int4, pad text);
INSERT INTO keyless SELECT g, repeat('x', 40) FROM generate_series(1, 20000) g;
CREATE INDEX keyless_v ON keyless (v);
ANALYZE costed, keyless});

# planned returns a statement's plan's total cost and its scan's rows, under
# settings that leave the scan of the kind asked for.
sub planned
{
	my ($sql, $settings) = @_;
	my $json = $node->safe_psql('postgres',
		"SET enable_seqscan = off; $settings; EXPLAIN (FORMAT JSON) $sql");
	my ($total) = $json =~ /"Total Cost": ([0-9.]+)/;
	my ($rows) = $json =~ /"Plan Rows": ([0-9]+)/;
	return ($total, $rows);
}

# shape returns a fork table's tree's leaves and height, and its tuple id
# tree's, as its scans' costs read them.
sub shape
{
	my ($table) = @_;
	return split / /,
	  $node->safe_psql('postgres',
		"SELECT doltgres_test_scan_shape('$table')");
}

# close_to says whether two costs agree within the 0.01 EXPLAIN rounds to.
sub close_to
{
	my ($got, $wanted) = @_;
	return abs($got - $wanted) < 0.015;
}

my $cpu_operator = 0.0025;
my $cpu_tuple = 0.01;
my $index_only = 'SET enable_bitmapscan = off';
my $index_scan =
  'SET enable_bitmapscan = off; SET enable_indexonlyscan = off';

# A secondary index whose table's leaves the node cache holds: the index scan
# costs the index-only scan's plus a descent's comparisons a row.
my ($leaves, $height, $tid_leaves, $tid_height) = shape('costed');
my ($only, $rows) =
  planned('SELECT v FROM costed WHERE v < 1000', $index_only);
my ($scan) = planned('SELECT v FROM costed WHERE v < 1000', $index_scan);
ok( close_to($scan - $only, $rows * $height * $cpu_operator),
	"a secondary index scan in the node cache costs a descent a row ($scan - $only, $rows rows, height $height)"
);

# The primary key's own index, whose leaves are the table's rows and fit the
# node cache, reads no more of the table than its index-only scan.
my ($key_only) =
  planned('SELECT id FROM costed WHERE id < 1000', $index_only);
my ($key_scan) =
  planned('SELECT id FROM costed WHERE id < 1000', $index_scan);
ok( close_to($key_scan, $key_only),
	"the primary key's own index scan in the node cache costs its index-only scan's ($key_scan, $key_only)"
);

# A bitmap heap scan's rows each cost a tuple id resolution and a descent,
# beside the recheck's CPU and cost_bitmap_tree_node's tenth of an operator
# for handling the bitmap, over its bitmap index scan's cost.
my $bitmap = $node->safe_psql('postgres',
	'SET enable_seqscan = off; SET enable_indexscan = off; EXPLAIN (FORMAT JSON) SELECT pad FROM costed WHERE v < 1000'
);
my ($heap_total, $index_total) =
  $bitmap =~ /"Total Cost": ([0-9.]+).*"Total Cost": ([0-9.]+)/s;
my ($bitmap_rows) = $bitmap =~ /"Plan Rows": ([0-9]+)/;
ok( close_to(
		$heap_total - $index_total,
		$bitmap_rows * (
			($tid_height + $height) * $cpu_operator +
			  $cpu_tuple +
			  $cpu_operator +
			  0.1 * $cpu_operator)),
	"a bitmap heap scan costs a tuple id resolution and a descent a row ($heap_total - $index_total, $bitmap_rows rows)"
);

# A keyless table's secondary index descends the table's tree by the row's
# hash, a descent a row as a keyed table's.
my ($keyless_leaves, $keyless_height) = shape('keyless');
my ($keyless_only, $keyless_rows) =
  planned('SELECT v FROM keyless WHERE v < 1000', $index_only);
my ($keyless_scan) =
  planned('SELECT v FROM keyless WHERE v < 1000', $index_scan);
ok( close_to(
		$keyless_scan - $keyless_only,
		$keyless_rows * $keyless_height * $cpu_operator),
	"a keyless table's secondary index scan costs a descent by the row's hash a row"
);

# An index-only scan reads no row of the table, so a VACUUM, which heap's
# visibility map would change it by, leaves its cost as it was.
$node->safe_psql('postgres', 'VACUUM costed');
my ($vacuumed) = planned('SELECT v FROM costed WHERE v < 1000', $index_only);
ok( close_to($vacuumed, $only),
	"an index-only scan's cost is the same before and after a VACUUM ($only, $vacuumed)"
);

# default_cost returns a statement's plan's total cost at default settings.
sub default_cost
{
	my ($sql) = @_;
	my ($total) =
	  $node->safe_psql('postgres', "EXPLAIN (FORMAT JSON) $sql") =~
	  /"Total Cost": ([0-9.]+)/;
	return $total;
}

# Item 16's plan choices at default settings, as the fork's own expected
# plans: before any VACUUM an index-only scan, whose table part is nothing;
# an index scan over a single-index bitmap scan, whose rows each cost a tuple
# id resolution beside the descent; and the primary key's range scan, its
# leaves the rows in order.
$node->safe_psql(
	'postgres', q{
CREATE TABLE fresh (id int4 PRIMARY KEY, v int4, pad text);
INSERT INTO fresh SELECT g, g, repeat('z', 40) FROM generate_series(1, 20000) g;
CREATE INDEX fresh_v ON fresh (v);
ANALYZE fresh});
my ($fresh_leaves, $fresh_height) = shape('fresh');
is( $node->safe_psql(
		'postgres', 'EXPLAIN (COSTS OFF) SELECT v FROM fresh WHERE v < 100'),
	"Index Only Scan using fresh_v on fresh\n  Index Cond: (v < 100)",
	'an index-only scan is chosen before any VACUUM');
my ($fresh_scan, $fresh_rows) =
  planned('SELECT v FROM fresh WHERE v < 100', $index_scan);
ok( close_to(
		default_cost('SELECT v FROM fresh WHERE v < 100'),
		$fresh_scan - $fresh_rows * $fresh_height * $cpu_operator),
	'the chosen index-only scan costs the index scan less its descents');
is( $node->safe_psql(
		'postgres',
		'EXPLAIN (COSTS OFF) SELECT pad FROM fresh WHERE v < 1000'),
	"Index Scan using fresh_v on fresh\n  Index Cond: (v < 1000)",
	'an index scan is chosen over a single-index bitmap scan');
my ($fresh_index) =
  planned('SELECT pad FROM fresh WHERE v < 1000', $index_scan);
my ($fresh_bitmap) = planned('SELECT pad FROM fresh WHERE v < 1000',
	'SET enable_indexscan = off');
ok( close_to(default_cost('SELECT pad FROM fresh WHERE v < 1000'),
		$fresh_index)
	  && $fresh_index < $fresh_bitmap,
	"the chosen index scan costs less than the bitmap scan ($fresh_index, $fresh_bitmap)"
);
is( $node->safe_psql(
		'postgres',
		'EXPLAIN (COSTS OFF) SELECT * FROM fresh WHERE id BETWEEN 100 AND 300'
	),
	"Index Scan using fresh_pkey on fresh\n  Index Cond: ((id >= 100) AND (id <= 300))",
	'the primary key\'s range scan is chosen');
my ($fresh_key_only) =
  planned('SELECT id FROM fresh WHERE id BETWEEN 100 AND 300', $index_only);
ok( close_to(
		default_cost('SELECT * FROM fresh WHERE id BETWEEN 100 AND 300'),
		$fresh_key_only),
	'the chosen key range scan in the node cache costs its index-only scan\'s'
);

# leaves_read is Mackert and Lohman's count of a tree's leaves that fetches
# read past a cache of so many leaves, as the fork's cost counts them.
sub leaves_read
{
	my ($fetches, $leaves, $cached) = @_;
	return 0 if $leaves <= $cached;
	my $limit = 2 * $leaves * $cached / (2 * $leaves - $cached);
	return $fetches <= $limit
	  ? ceil(2 * $leaves * $fetches / (2 * $leaves + $fetches))
	  : ceil($cached + ($fetches - $limit) * ($leaves - $cached) / $leaves);
}

# Past the node cache, a secondary index scan also reads Mackert and Lohman's
# count of the table's leaves at random, with the node cache in
# effective_cache_size's place.
$node->append_conf('postgresql.conf', 'node_cache_size = 1024kB');
$node->restart;
$node->safe_psql(
	'postgres', q{
CREATE TABLE wide (id int4 PRIMARY KEY, v int4, pad text);
INSERT INTO wide SELECT g, (g * 7919) % 60000, repeat('y', 400) FROM generate_series(1, 60000) g;
CREATE INDEX wide_v ON wide (v);
ANALYZE wide});
my ($wide_leaves, $wide_height) = shape('wide');
my $cached = 1024 * 1024 / 8192;
ok( $wide_leaves > $cached,
	"the wide table's leaves ($wide_leaves) exceed the node cache's $cached");
my ($wide_only, $wide_rows) =
  planned('SELECT v FROM wide WHERE v < 3000', $index_only);
my ($wide_scan) = planned('SELECT v FROM wide WHERE v < 3000', $index_scan);
my $read = leaves_read($wide_rows, $wide_leaves, $cached);
ok( close_to(
		$wide_scan - $wide_only,
		$wide_rows * $wide_height * $cpu_operator + $read * 4),
	"a secondary index scan past the node cache reads Mackert and Lohman's leaves at random ($wide_scan - $wide_only, $wide_rows rows, $read leaves)"
);

# The primary key's own index past the node cache reads its leaves in key
# order, the first at random and the rest in sequence: the selectivity's
# share of the leaves, which the plan's rows give to within a row.
my ($wide_key_only, $wide_key_rows) =
  planned('SELECT id FROM wide WHERE id <= 3000', $index_only);
my ($wide_key_scan) =
  planned('SELECT id FROM wide WHERE id <= 3000', $index_scan);
my $wide_tuples = $node->safe_psql('postgres',
	"SELECT reltuples FROM pg_class WHERE relname = 'wide'");
my @key_reads = map { ceil($_ / $wide_tuples * $wide_leaves) }
  ($wide_key_rows - 0.5, $wide_key_rows + 0.5);
ok( (   grep { close_to($wide_key_scan - $wide_key_only, 4 + ($_ - 1)) }
		  @key_reads),
	"the primary key's own index scan past the node cache reads its leaves in order ($wide_key_scan - $wide_key_only, reads @key_reads)"
);

# A repeated inner index scan, a nested loop's parameterized one, costs each
# loop its share of Mackert and Lohman's leaves over every loop's fetches: a
# secondary index's rows and the primary key's one leaf a probe. Enough
# probes pass the node cache's share of the leaves that the count differs
# from heap's over effective_cache_size.
$node->safe_psql('postgres',
	'CREATE TABLE probes (x int4); INSERT INTO probes SELECT g * 13 FROM generate_series(1, 4000) g; ANALYZE probes'
);
my $nested =
  'SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_material = off; SET enable_memoize = off; SET enable_bitmapscan = off';

# inner returns a nested loop's inner index scan's cost a loop, its rows a
# loop, and the loops, its outer scan's rows.
sub inner
{
	my ($sql, $settings) = @_;
	my $json = $node->safe_psql('postgres',
		"$nested; $settings; EXPLAIN (FORMAT JSON) $sql");
	my ($loops) = $json =~ /"Node Type": "Seq Scan".*?"Plan Rows": ([0-9]+)/s;
	my ($cost, $rows) = $json =~
	  /"Node Type": "Index (?:Only )?Scan".*?"Total Cost": ([0-9.]+),\s*"Plan Rows": ([0-9]+)/s;
	return ($cost, $rows, $loops);
}
my ($loop_only, $loop_rows, $loops) =
  inner('SELECT w.v FROM probes p JOIN wide w ON w.v = p.x', '');
my ($loop_scan) = inner('SELECT w.pad FROM probes p JOIN wide w ON w.v = p.x',
	'SET enable_indexonlyscan = off');
ok( close_to(
		$loop_scan - $loop_only,
		$loop_rows *
		  $wide_height *
		  $cpu_operator +
		  leaves_read($loop_rows * $loops, $wide_leaves, $cached) * 4 / $loops
	),
	"a repeated secondary index scan costs its loop's share of the leaves read ($loop_scan - $loop_only, $loops loops)"
);
my ($key_loop_only) =
  inner('SELECT w.id FROM probes p JOIN wide w ON w.id = p.x', '');
my ($key_loop_scan) =
  inner('SELECT w.pad FROM probes p JOIN wide w ON w.id = p.x',
	'SET enable_indexonlyscan = off');
ok( close_to(
		$key_loop_scan - $key_loop_only,
		leaves_read($loops, $wide_leaves, $cached) * 4 / $loops),
	"a repeated primary key index scan costs its loop's share of the leaves read ($key_loop_scan - $key_loop_only, $loops loops)"
);

# A lossy bitmap: with 64kB of work_mem the bitmap holds 1024 entries, the
# size of a page's entry and two pointers, so of the pages compute_bitmap_pages
# fetches all but half the entries are lossy, and each costs a range scan of
# the tuple id tree: a descent, and its leaf read at random past the node
# cache. A scan of every row keeps the rows fetched the same either way.
my ($wide_tid_leaves, $wide_tid_height) = (shape('wide'))[ 2, 3 ];
my $lossy_sql = 'SELECT pad FROM wide WHERE v >= 0';
my ($lossy_small) =
  planned($lossy_sql, "SET enable_indexscan = off; SET work_mem = '64kB'");
my ($lossy_large) =
  planned($lossy_sql, "SET enable_indexscan = off; SET work_mem = '64MB'");
my $wide_pages = $node->safe_psql('postgres',
	"SELECT relpages FROM pg_class WHERE relname = 'wide'");
my $fetched =
  2 * $wide_pages * $wide_tuples / (2 * $wide_pages + $wide_tuples);
$fetched = $fetched >= $wide_pages ? $wide_pages : ceil($fetched);
my $lossy = $fetched - 1024 / 2;
ok( $lossy > 0 && close_to(
		$lossy_small - $lossy_large,
		$lossy * (
			$wide_tid_height * $cpu_operator +
			  ($wide_tid_leaves > $cached ? 4 : 0))),
	"a lossy bitmap's blocks each cost a tuple id range scan ($lossy_small - $lossy_large, $lossy lossy blocks)"
);

$node->stop;
done_testing();
