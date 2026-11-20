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

# The comparator of a table keyed by a column its btree class compares by a
# function (the access map's A4), through the test module: each table's base
# is saved by doltgres_test_base, read back by doltgres_test_scan in its tree's
# order, and fetched by doltgres_test_fetch by each row's ctid. Each
# expectation is the heap table's own rows in Postgres' ORDER BY.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('compared');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

# rows returns a scan's rows' columns, each row's ctid and xmin left off.
sub rows
{
	my ($target) = @_;
	return $node->safe_psql('postgres',
		"SELECT substring(r from '^\\S+ \\S+ (.*)\$') FROM doltgres_test_scan('$target', false, -1) r"
	);
}

# saved saves a heap table's rows as a fork table's base, both made by a
# statement.
sub saved
{
	my ($statements, $target, $source) = @_;
	$node->safe_psql('postgres',
		"$statements SELECT doltgres_test_base('$target', '$source');");
}

saved(
	q{CREATE TABLE source_numeric (a numeric, b text);
INSERT INTO source_numeric VALUES ('10', 'ten'), ('1.5', 'one'), ('9.75e1', 'big'), ('-3', 'less'), ('0.001', 'small');
CREATE TABLE by_numeric (a numeric PRIMARY KEY, b text);},
	'by_numeric', 'source_numeric');
is( rows('by_numeric'),
	$node->safe_psql(
		'postgres', q{SELECT a || ' ' || b FROM source_numeric ORDER BY a}),
	"a table keyed by a numeric scans in numeric order");
foreach my $row (
	split /\n/,
	$node->safe_psql(
		'postgres',
		q{SELECT r FROM doltgres_test_scan('by_numeric', false, -1) r}))
{
	my ($tid) = split / /, $row;
	is( $node->safe_psql(
			'postgres', "SELECT doltgres_test_fetch('by_numeric', '$tid')"),
		$row,
		"a row of a table keyed by a numeric fetches by its ctid");
}

saved(
	q{CREATE TABLE source_mixed (a numeric, b text COLLATE "C");
INSERT INTO source_mixed VALUES ('2', 'b'), ('2.0', 'a'), ('10', 'a'), ('1', 'z'), ('1', 'B');
CREATE TABLE by_mixed (a numeric, b text COLLATE "C", PRIMARY KEY (b, a));},
	'by_mixed', 'source_mixed');
is( rows('by_mixed'),
	$node->safe_psql(
		'postgres', q{SELECT a || ' ' || b FROM source_mixed ORDER BY b, a}),
	"a table keyed by a byte column and a compared column orders by both");

saved(
	q{CREATE TYPE pair AS (n numeric, t text);
CREATE TABLE source_pair (p pair);
INSERT INTO source_pair SELECT ROW(n * 1.5, 'r' || n)::pair FROM generate_series(200, 1, -1) n;
CREATE TABLE by_pair (p pair PRIMARY KEY);},
	'by_pair', 'source_pair');
is( rows('by_pair'),
	$node->safe_psql('postgres', q{SELECT p FROM source_pair ORDER BY p}),
	"a table keyed by a row type holding a numeric orders as the row type does"
);

saved(
	q{CREATE TABLE source_array (a numeric[]);
INSERT INTO source_array VALUES ('{10}'), ('{9,1}'), ('{9.5}'), ('{-1,2}'), ('{}');
CREATE TABLE by_array (a numeric[] PRIMARY KEY);},
	'by_array', 'source_array');
is( rows('by_array'),
	$node->safe_psql('postgres', q{SELECT a FROM source_array ORDER BY a}),
	"a table keyed by an array of numerics orders as the array does");

# Each comparison runs in a memory context of its own, so a function that
# caches in its call's memory, as record_cmp does, leaves nothing behind.
my ($last) = split / /,
  $node->safe_psql('postgres',
	q{SELECT r FROM doltgres_test_scan('by_pair', true, -1) r LIMIT 1});
is( $node->safe_psql(
		'postgres',
		"SELECT doltgres_test_fetch_memory('by_pair', '$last', 1000)"),
	'0',
	"many comparisons through a row-type key leave the entry's context flat");

# A comparison function that reads a fork table brings a nested entry into the
# fork while the outer comparison runs; the nested read gives its row, and the
# outer comparisons order the table as Postgres does.
saved(
	q{CREATE TABLE source_lookup (a int4, b text);
INSERT INTO source_lookup VALUES (1, 'found');
CREATE TABLE lookup (a int4 PRIMARY KEY, b text);},
	'lookup', 'source_lookup');
saved(
	q{CREATE FUNCTION nested_cmp(x int4, y int4) RETURNS int4 LANGUAGE sql AS $$
  SELECT CASE WHEN doltgres_test_fetch('lookup', '(0,1)') LIKE '% found' THEN btint4cmp(x, y) END
$$;
CREATE OPERATOR CLASS nested_ops FOR TYPE int4 USING btree AS
  OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >,
  FUNCTION 1 nested_cmp(int4, int4);
CREATE TYPE nested_range AS RANGE (subtype = int4, subtype_opclass = nested_ops);
CREATE TABLE source_nested (r nested_range);
INSERT INTO source_nested SELECT nested_range(n % 17, n % 17 + n) FROM generate_series(1, 40) n;
CREATE TABLE by_nested (r nested_range PRIMARY KEY);},
	'by_nested', 'source_nested');
is( rows('by_nested'),
	$node->safe_psql('postgres', q{SELECT r FROM source_nested ORDER BY r}),
	"a comparison function that reads a fork table orders the table as Postgres does"
);

$node->stop;
done_testing();
