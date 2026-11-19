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

# The fork's TID range scans over a storage's base (PLAN item 2), through the
# test module: doltgres_test_base saves a heap table's rows as another
# table's base, its rows taking the ctids heap gives them, and
# doltgres_test_tidrange reads a range of them through the fork's table
# access method routine, forward or backward. Each expectation is Postgres':
# heap's own tid range scan of the source's rows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('tidrange');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

# ranged returns a range scan's rows, each row's xmin left off.
sub ranged
{
	my ($target, $min, $max, $backward) = @_;
	return $node->safe_psql('postgres',
		"SELECT regexp_replace(r, '^(\\S+) \\S+ ', '\\1 ') FROM doltgres_test_tidrange('$target', '$min', '$max', $backward) r"
	);
}

# expected returns a source's rows whose ctids are in a range, as heap's own
# tid range scan gives them, in ctid order or its reverse, each as its ctid
# and its columns' text.
sub expected
{
	my ($source, $columns, $min, $max, $backward) = @_;
	my $order = $backward eq 'true' ? 'DESC' : 'ASC';
	return $node->safe_psql('postgres',
		"SELECT ctid || ' ' || $columns FROM $source WHERE ctid BETWEEN '$min' AND '$max' ORDER BY ctid $order"
	);
}

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_many (a int4, b text);
INSERT INTO source_many SELECT n, 'r' || n FROM generate_series(1, 600) n;
CREATE TABLE many (a int4 PRIMARY KEY, b text);
SELECT doltgres_test_base('many', 'source_many');
});

my %names = (
	'(0,10) (0,20)' =>
	  "a range within the first block gives its rows in ctid order",
	"(0,280) (2,5)" =>
	  "a range across two block boundaries gives its rows in ctid order",
	'(0,400) (1,5)' =>
	  "a range whose offsets pass a page's most tuples gives the rows it holds",
	'(1,0) (1,3)' => "a range from offset 0 begins at its block's first row",
	'(0,5) (1,0)' =>
	  "a range to offset 0 ends at the block before's last row",
	'(1,5) (1,1)' =>
	  "a range whose first ctid is above its last gives no row",
	'(9,1) (10,1)' => "a range past the table's end gives no row");
foreach my $range (sort keys %names)
{
	my ($min, $max) = split / /, $range;
	foreach my $backward ('false', 'true')
	{
		is( ranged('many', $min, $max, $backward),
			expected('source_many', "a || ' ' || b", $min, $max, $backward),
			$names{$range});
	}
}

is( $node->safe_psql(
		'postgres',
		q{SELECT regexp_replace(r, '^(\S+) \S+ ', '\1 ') FROM doltgres_test_tidrange_again('many', '(0,1)', '(0,5)', '(1,1)', '(1,4)') r}
	),
	expected('source_many', "a || ' ' || b", '(1,1)', '(1,4)', 'false'),
	"a rescan and a new range read the new range");

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_keyless (a int4, b text);
INSERT INTO source_keyless VALUES (1, 'x'), (2, 'y'), (1, 'x'), (1, 'x');
CREATE TABLE keyless (a int4, b text);
SELECT doltgres_test_base('keyless', 'source_keyless');
});
foreach my $backward ('false', 'true')
{
	is( ranged('keyless', '(0,1)', '(0,4)', $backward),
		expected(
			'source_keyless', "a || ' ' || b",
			'(0,1)', '(0,4)', $backward),
		"each copy of an equal keyless row is in a range once with its own ctid"
	);
}

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_numeric (a numeric, b text);
INSERT INTO source_numeric VALUES ('10', 'ten'), ('1.5', 'one'), ('9.75e1', 'big'), ('-3', 'less'), ('0.001', 'small');
CREATE TABLE by_numeric (a numeric PRIMARY KEY, b text);
SELECT doltgres_test_base('by_numeric', 'source_numeric');
});
foreach my $backward ('false', 'true')
{
	is( ranged('by_numeric', '(0,2)', '(0,5)', $backward),
		expected(
			'source_numeric', "a || ' ' || b",
			'(0,2)', '(0,5)', $backward),
		"a range of a table keyed by a numeric resolves its rows through the comparator"
	);
}

$node->stop;
done_testing();
