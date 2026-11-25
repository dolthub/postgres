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

# Reads of an unfolded table whose rows spilled: a key fetch checks only its
# own key's spilled versions, not every version of the chunk that holds it,
# whether the keys it fetches scatter over chunks or share one, which it walks
# once; and a sequential read builds each spill file's name once, not once a
# row (bugs rows 479 and 429).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $rows = 20000;
my $fetched = 100;
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"delta_memory = 16MB\nfold_threshold = 1GB\nfold_timeout = 3600s");
$node->start;

# The load fits the delta, and its writer spills its own versions before it
# commits, a file of each tree at once, so the spills' files are few and
# fixed, and the delta holds nothing for a read below to spill or merge while
# its reads are counted.
my $relieved = $node->safe_psql('postgres',
	"CREATE EXTENSION doltgres_test; CREATE TABLE spilled (k int4 PRIMARY KEY, v int4, w int4, pad text); CREATE INDEX ON spilled (v); CREATE INDEX ON spilled (w); BEGIN; INSERT INTO spilled SELECT g, g, ((g::int8 * 7919) % $rows)::int4, repeat('x', 100) FROM generate_series(1, $rows) g; SELECT count(*) FROM generate_series(1, 1000) WHERE doltgres_test_delta(61, 0, 0) = 1; COMMIT"
);
note("the load's writer spilled $relieved times before its commit");
cmp_ok($relieved, '<', 1000, 'the load\'s writer has nothing left to spill');
my @manifests =
  glob($node->data_dir . '/pg_versioned/*/delta/spill/spills-*');
ok(@manifests > 0, 'the load spilled its rows');

# The counts an index read of the rows whose column is at most a bound adds:
# the spilled versions it checks and the chunks it walks whole.
sub counted
{
	my ($column, $bound) = @_;
	my ($checked, $walked, $count, $checked_after, $walked_after) =
	  split /\n/,
	  $node->safe_psql(
		'postgres',
		"SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off; SELECT doltgres_test_spill_reads(0); SELECT doltgres_test_spill_reads(2); SELECT count(*) FROM spilled WHERE $column <= $bound; SELECT doltgres_test_spill_reads(0); SELECT doltgres_test_spill_reads(2)"
	  );
	return ($count, $checked_after - $checked, $walked_after - $walked);
}

# Clustered keys, v = k: an index read of many rows and one of a single row
# read the same chunk of the table's spills, so the many-row read checks only
# its further keys' versions and walks no further chunk.
my ($count, $checked_one, $walked_one) = counted('v', 1);
is($count, 1, 'the clustered read of one row reads it');
my ($checked_many, $walked_many);
($count, $checked_many, $walked_many) = counted('v', $fetched);
is($count, $fetched, 'the clustered read reads every row it selects');
note(
	"the clustered further ",
	$fetched - 1,
	" key fetches checked ",
	$checked_many - $checked_one,
	' spilled versions and walked ',
	$walked_many - $walked_one,
	' chunks');
cmp_ok($walked_many - $walked_one,
	'==', 0, 'clustered key fetches walk their shared chunk once (row 479)');
cmp_ok(
	$checked_many - $checked_one,
	'<=',
	2 * ($fetched - 1),
	'clustered key fetches check their own keys\' versions alone (row 479)');

# Scattered keys, w a permutation of k: each key fetch reads another chunk,
# and checks only its own key's versions in it.
($count, $checked_one, $walked_one) = counted('w', 0);
is($count, 1, 'the scattered read of one row reads it');
($count, $checked_many, $walked_many) = counted('w', $fetched - 1);
is($count, $fetched, 'the scattered read reads every row it selects');
note(
	"the scattered further ",
	$fetched - 1,
	" key fetches checked ",
	$checked_many - $checked_one,
	' spilled versions');
cmp_ok(
	$checked_many - $checked_one,
	'<',
	3 * ($fetched - 1),
	'scattered key fetches check their own keys\' versions, not their chunks\' (row 479)'
);

# The primary key, k: the index read's own steps read the table's spills in
# order, and each row's key fetch finds the chunk those steps keep, so a read
# of many rows walks each chunk once.
($count, $checked_one, $walked_one) = counted('k', 1);
is($count, 1, 'the primary key\'s read of one row reads it');
($count, $checked_many, $walked_many) = counted('k', 10 * $fetched);
is( $count,
	10 * $fetched,
	'the primary key\'s read reads every row it selects');
note(
	"the primary key's further ",
	10 * $fetched - 1,
	" rows walked ",
	$walked_many - $walked_one,
	' chunks');
cmp_ok($walked_many - $walked_one, '<', $fetched,
	'the primary key\'s read walks each chunk once, not once a row (row 479)'
);

my ($before, $read, $after) = split /\n/,
  $node->safe_psql('postgres',
	"SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off; SELECT doltgres_test_spill_reads(1); SELECT count(*) FROM spilled; SELECT doltgres_test_spill_reads(1)"
  );
note(
	"the sequential read of $rows rows built ",
	$after - $before,
	' spill files\' names');
is($read, $rows, 'the sequential read reads every row');
cmp_ok(
	$after - $before,
	'<',
	$rows / 10,
	'the sequential read builds each spill file\'s name once, not once a row (row 429)'
);

$node->stop;

done_testing();
