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

# A table's size for costs (PLAN item 5), through the test module:
# doltgres_test_base saves a heap table's rows as another table's base, and
# doltgres_test_blocks reads the fork table's block count as Postgres does,
# RelationGetNumberOfBlocks. Each expected count is item 5's formula over
# Postgres' own heap of the same rows: each row's lp_len from pageinspect,
# MAXALIGNed with its line pointer, packed uniformly at their mean into a
# page's usable bytes, and at least the blocks the rows' ctids name. Heap's
# own page count is checked beside it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('sizes');
$node->init;
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins(); CREATE EXTENSION pageinspect');

# expected returns item 5's block count for a heap source's rows at a
# fillfactor, from its pages' line pointers.
sub expected
{
	my ($source, $fillfactor) = @_;
	return $node->safe_psql(
		'postgres', qq{
SELECT CASE WHEN count(*) = 0 THEN 0 ELSE greatest(
  ceil(count(*)::numeric / greatest(1, floor((8192 - 24 - 8192 * (100 - $fillfactor) / 100) / ceil(sum((lp_len + 7) / 8 * 8 + 4)::numeric / count(*))))),
  ceil(count(*)::numeric / 291)) END
FROM generate_series(0, greatest(pg_relation_size('$source') / 8192 - 1, -1)) p,
  heap_page_items(get_raw_page('$source', p::int))
WHERE lp_len > 0});
}

# heap_pages returns heap's own page count for a table.
sub heap_pages
{
	my ($source) = @_;
	return $node->safe_psql('postgres',
		"SELECT pg_relation_size('$source') / 8192");
}

# blocks returns the fork's block count for a table.
sub blocks
{
	my ($target) = @_;
	return $node->safe_psql('postgres',
		"SELECT doltgres_test_blocks('$target')");
}

# saved makes a heap source and a fork target of the same columns and options,
# fills the source by a statement, and saves its rows as the target's base.
sub saved
{
	my ($name, $columns, $with, $insert) = @_;
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE source_$name ($columns) $with;
CREATE TABLE $name ($columns) $with;
INSERT INTO source_$name $insert;
SELECT doltgres_test_base('$name', 'source_$name');
});
}

saved(
	'narrow', 'a int4, b text',
	'', q{SELECT n, 'rrrr' FROM generate_series(1, 5000) n});
is( blocks('narrow'),
	expected('source_narrow', 100),
	'a table of narrow rows reports the blocks its heap bytes fill');
is( heap_pages('source_narrow'),
	expected('source_narrow', 100),
	'heap packs the narrow rows as the formula does');

saved(
	'wide', 'a int4, b text',
	'', q{SELECT n, repeat('x', 1900) FROM generate_series(1, 300) n});
is( blocks('wide'),
	expected('source_wide', 100),
	'a table of wide rows reports the blocks its heap bytes fill');
is(heap_pages('source_wide'), '75', 'heap fills 75 pages with the wide rows');
is(blocks('wide'), '75', 'the fork counts the 75 pages heap fills');

saved(
	'half',
	'a int4, b text',
	'WITH (fillfactor = 50)',
	q{SELECT n, repeat('x', 200) FROM generate_series(1, 1000) n});
is( blocks('half'),
	expected('source_half', 50),
	'a table at fillfactor 50 reports the blocks heap fills at it');
is( heap_pages('source_half'),
	expected('source_half', 50),
	'heap fills the same blocks at fillfactor 50');

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_no_columns ();
CREATE TABLE no_columns ();
INSERT INTO source_no_columns SELECT FROM generate_series(1, 292);
SELECT doltgres_test_base('no_columns', 'source_no_columns');
});
is(blocks('no_columns'), '2', "rows of no columns report their ctid blocks");
is(heap_pages('source_no_columns'),
	'2', 'heap fills two blocks with the same rows');

saved('mixed', 'a int4, b text',
	'', q{SELECT n, repeat('x', n % 500) FROM generate_series(1, 2000) n});
is( blocks('mixed'),
	expected('source_mixed', 100),
	'a table of mixed widths reports the formula over its heap bytes');
cmp_ok(
	heap_pages('source_mixed'),
	'>=',
	expected('source_mixed', 100),
	"heap's pages for mixed widths are at least the formula's");

# A 4,000-byte value moves out of line in the fork by the storage design's
# II.9, counted as heap's 18-byte toast pointer, while heap compresses it
# inline: each fork row takes MAXALIGN(24 + 4 + 18) + 4 = 52 bytes, so the
# counts differ, as PLAN's Differences row 37 records.
saved('compressible', 'a int4, b text',
	'', q{SELECT n, repeat('x', 4000) FROM generate_series(1, 500) n});
is( blocks('compressible'),
	$node->safe_psql(
		'postgres', 'SELECT ceil(500::numeric / floor((8192 - 24) / 52))'),
	'a compressible wide row counts as its toast pointer');
note(
	'heap pages for the compressible rows: ',
	heap_pages('source_compressible'));

$node->safe_psql('postgres', 'CREATE TABLE empty_table (a int4)');
is(blocks('empty_table'), '0', 'an empty table reports no blocks');

is( $node->safe_psql(
		'postgres', q{SELECT doltgres_test_estimate('narrow', false)}),
	$node->safe_psql(
		'postgres', q{SELECT doltgres_test_estimate('source_narrow', true)}),
	"the estimate gives heap's pages and tuples for the same rows");

$node->stop;
done_testing();
