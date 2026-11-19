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

# The fork's table scans over a storage's base (design item 13, path step 3),
# through the test module: doltgres_test_base saves a heap table's rows as
# another table's base, and doltgres_test_scan reads that table through the
# fork's table access method routine, forward or backward, with a key or
# none, giving each row's ctid, xmin, and columns. Each expectation is the
# heap table's own rows in the key's order.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('scan');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

# rows returns a scan's rows' columns, each row's ctid and xmin left off.
sub rows
{
	my ($target, $backward, $equals) = @_;
	return $node->safe_psql('postgres',
		"SELECT substring(r from '^\\S+ \\S+ (.*)\$') FROM doltgres_test_scan('$target', $backward, $equals) r"
	);
}

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_one (a int4, b text);
INSERT INTO source_one VALUES (3, 'c'), (1, 'a'), (2, 'b');
CREATE TABLE one (a int4 PRIMARY KEY, b text);
SELECT doltgres_test_base('one', 'source_one');
});
is( rows('one', 'false', -1),
	$node->safe_psql(
		'postgres', q{SELECT a || ' ' || b FROM source_one ORDER BY a}),
	"a seq scan of a table on the fork's storage gives its base's rows");
is( rows('one', 'true', -1),
	$node->safe_psql(
		'postgres', q{SELECT a || ' ' || b FROM source_one ORDER BY a DESC}),
	"a backward scan gives the rows in the reverse of the key's order");

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_later (a int4, b text COLLATE "C", c int8);
INSERT INTO source_later VALUES (1, 'b', 7), (2, 'a', 7), (3, 'B', 5), (4, 'a', 5);
CREATE TABLE later (a int4, b text COLLATE "C", c int8, PRIMARY KEY (c, b));
SELECT doltgres_test_base('later', 'source_later');
});
is( rows('later', 'false', -1),
	$node->safe_psql(
		'postgres',
		q{SELECT a || ' ' || b || ' ' || c FROM source_later ORDER BY c, b}),
	"a scan of a table keyed by later columns gives its rows in key order");

is( rows('later', 'false', 3),
	$node->safe_psql(
		'postgres',
		q{SELECT a || ' ' || b || ' ' || c FROM source_later WHERE a = 3}),
	"a scan with keys gives the rows they match, as heap's does");

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_keyless (a int4, b text);
INSERT INTO source_keyless VALUES (1, 'x'), (1, 'x'), (2, 'y');
CREATE TABLE keyless (a int4, b text);
SELECT doltgres_test_base('keyless', 'source_keyless');
});
my @copies = split /\n/,
  $node->safe_psql('postgres',
	q{SELECT r FROM doltgres_test_scan('keyless', false, 1) r});
is(scalar(@copies), 2, "two equal rows of a keyless table read back as two");
my ($first, $second) = map { (split / /)[0] } @copies;
isnt($first, $second, "each copy of an equal row has its own ctid");

# fetch returns the row a fetch by a ctid gives, its ctid first, or an empty
# string for none.
sub fetch
{
	my ($target, $tid) = @_;
	return $node->safe_psql('postgres',
		"SELECT coalesce(doltgres_test_fetch('$target', '$tid'), '')");
}

# A page's most tuples and the blocks they fill, as heap gives them for rows
# of no columns, which it packs to that most.
$node->safe_psql(
	'postgres', q{
CREATE TABLE no_columns ();
INSERT INTO no_columns SELECT FROM generate_series(1, 292);
});
my $most = $node->safe_psql('postgres',
	q{SELECT max((ctid::text::point)[1]) FROM no_columns WHERE (ctid::text::point)[0] = 0}
);
my $blocks = $node->safe_psql('postgres',
	q{SELECT pg_relation_size('no_columns') / current_setting('block_size')::int8}
);

# A table's rows take the ctids heap gives the same rows, packed onto pages by
# their heap bytes, so the row a ctid fetches is the source's row of that
# ctid: the first row, the last of the first block, the first of the next, and
# the last.
$node->safe_psql(
	'postgres', q{
CREATE TABLE source_many (a int4, b text);
INSERT INTO source_many SELECT n, 'r' || n FROM generate_series(1, 292) n;
CREATE TABLE many (a int4 PRIMARY KEY, b text);
SELECT doltgres_test_base('many', 'source_many');
});
my $last_of_first = $node->safe_psql('postgres',
	q{SELECT max(ctid) FROM source_many WHERE ctid < '(1,0)'});
my $last = $node->safe_psql('postgres', q{SELECT max(ctid) FROM source_many});
foreach my $tid ('(0,1)', $last_of_first, '(1,1)', $last)
{
	is( fetch('many', $tid) =~ s/^(\S+) \S+ /$1 /r,
		$node->safe_psql(
			'postgres',
			"SELECT '$tid ' || a || ' ' || b FROM source_many WHERE ctid = '$tid'"
		),
		"a row fetched by its ctid is the row in that place of heap's order");
}
my ($last_block, $last_offset) = $last =~ /\((\d+),(\d+)\)/;
foreach my $tid (
	"($last_block," . ($last_offset + 1) . ")", '(0,0)',
	"(0," . ($most + 1) . ")", '(5,1)')
{
	is(fetch('many', $tid), '', "a ctid no row has gives none");
}

foreach my $copy (@copies)
{
	my ($tid) = split / /, $copy;
	is(fetch('keyless', $tid),
		$copy, "each copy of an equal keyless row fetches by its own ctid");
}

# A ctid is valid below the storage's count of blocks, as many as heap fills
# with as many rows at a page's most tuples, with an offset from 1 to that
# most.
foreach my $case (
	[ "(0,1)", 't' ],
	[ "(" . ($blocks - 1) . ",$most)", 't' ],
	[ "($blocks,1)", 'f' ],
	[ '(0,0)', 'f' ],
	[ "(0," . ($most + 1) . ")", 'f' ])
{
	my ($tid, $valid) = @$case;
	is( $node->safe_psql(
			'postgres', "SELECT doltgres_test_tid_valid('many', '$tid')"),
		$valid,
		"a ctid is valid only within the storage's blocks");
}

# Over a base alone, a fetched row is visible to the snapshot that reads it,
# and its latest version is itself.
isnt(fetch('many', '(1,1)'),
	'not visible', "a fetched row satisfies the snapshot that reads it");
is( $node->safe_psql(
		'postgres', q{SELECT doltgres_test_latest('many', '(1,1)')}),
	'(1,1)',
	"a fetched row's latest tid is its own");

# A transaction that read a storage ends its record of held states at its
# commit, so its resource owner is left none to warn of; so does a parallel
# worker's commit, here a worker that runs the scan under
# debug_parallel_query, since no table in SQL uses the fork's routine yet.
like(
	$node->safe_psql(
		'postgres', q{
ALTER FUNCTION doltgres_test_scan PARALLEL SAFE;
SET debug_parallel_query = on;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) FROM doltgres_test_scan('many', false, 0);
}),
	qr/Workers Launched: 1/,
	"a parallel worker runs a scan of the fork's routine");
ok(!$node->log_contains('resource was not closed: versioned owned record'),
	"a scan's transaction commits with no record left to its resource owner");

# An index fetch by a ctid gives the row a fetch of that version gives, the
# same after a reset, and never asks to be called again, since a ctid names
# one version; a ctid no row has gives none.
foreach my $tid ('(0,1)', $last_of_first, '(1,1)', $last)
{
	is( $node->safe_psql(
			'postgres', "SELECT doltgres_test_index_fetch('many', '$tid')"),
		fetch('many', $tid) . ' f f',
		"an index fetch by a ctid gives the row a fetch of its version gives"
	);
}
is( $node->safe_psql(
		'postgres', q{SELECT doltgres_test_index_fetch('many', '(5,1)')}),
	'none f f',
	"an index fetch by a ctid no row has gives none");

# An index fetch by a row key, the constant tid the fork's index method sets,
# gives the version of the key its snapshot sees and writes that version's
# tid: over a base, over rows a transaction inserted into the delta, and of a
# keyless table's copy its tuple id names; a key no row has gives none.
sub by_key
{
	my ($target, $source, $copy) = @_;
	return $node->safe_psql('postgres',
		"SELECT doltgres_test_index_fetch_key('$target', '$source', $copy)");
}
foreach my $line (split /\n/, by_key('one', 'source_one', 0))
{
	my ($ctid) = split / /, $line;
	is( $line,
		fetch('one', $ctid) . " $ctid",
		"an index fetch by a row key gives the version of the key the snapshot sees and writes its tid"
	);
}
$node->safe_psql(
	'postgres', q{
CREATE TABLE inserted (a int4 PRIMARY KEY, b text);
SELECT doltgres_test_insert('inserted', 'source_one', false, 0);
});
foreach my $line (split /\n/, by_key('inserted', 'source_one', 0))
{
	my ($ctid) = split / /, $line;
	is( $line,
		fetch('inserted', $ctid) . " $ctid",
		"an index fetch by a row key gives a version the delta holds and writes its tid"
	);
}
$node->safe_psql(
	'postgres', q{
CREATE TABLE source_copy (a int4, b text);
INSERT INTO source_copy VALUES (1, 'x');
CREATE TABLE source_missing (a int4, b text);
INSERT INTO source_missing VALUES (9, 'z');
});
my ($block, $offset) = $second =~ /\((\d+),(\d+)\)/;
is( by_key('keyless', 'source_copy', $block * 65536 + $offset),
	"$copies[1] $second",
	"an index fetch by a keyless row's key gives the copy its tuple id names"
);
is(by_key('one', 'source_missing', 0),
	'none', "an index fetch by a row key no row has gives none");

# A bitmap scan gives an exact page's rows by their tids, each as a fetch by its
# tid gives it, and a lossy page's every row with a recheck, passing over a page
# past the storage's blocks, and counts its pages lossy and exact, as heap's
# does.
my @exact = ('(0,1)', '(0,5)', '(1,1)');
is( $node->safe_psql(
		'postgres',
		"SELECT doltgres_test_bitmap('many', '{"
		  . join(',', map { "\"$_\"" } @exact)
		  . "}', '{}')"),
	join("\n", (map { fetch('many', $_) . ' exact' } @exact), 'pages 0 2'),
	"a bitmap scan gives an exact page's rows by their tids and a lossy page's rows with a recheck"
);
my @lossy = split /\n/,
  $node->safe_psql('postgres',
	q{SELECT ctid FROM source_many WHERE ctid >= '(1,0)' ORDER BY ctid});
is( $node->safe_psql(
		'postgres', q{SELECT doltgres_test_bitmap('many', '{}', '{1,5}')}),
	join("\n", (map { fetch('many', $_) . ' recheck' } @lossy), 'pages 1 0'),
	"a lossy page of a bitmap scan gives every row of its block with a recheck"
);

# TABLESAMPLE's methods pick ctid blocks and offsets, so over the fork's table,
# whose rows take heap's ctids, a repeatable sample gives the rows heap's gives
# over the same rows.
foreach
  my $method ('BERNOULLI (30) REPEATABLE (7)', 'SYSTEM (60) REPEATABLE (3)')
{
	is( $node->safe_psql(
			'postgres',
			"SELECT doltgres_test_standing('many', 'SELECT a FROM many TABLESAMPLE $method')"
		),
		$node->safe_psql(
			'postgres', "SELECT a FROM source_many TABLESAMPLE $method"),
		"TABLESAMPLE over the fork's table gives heap's rows of the same ctids"
	);
}

# ANALYZE's block sampler picks ctid blocks of the table's size, so over the
# fork's table, whose rows take heap's ctids and whose size is heap's, its
# sample is heap's over the same rows, every row live.
is( $node->safe_psql(
		'postgres', q{SELECT doltgres_test_analyze('many', true, 100, 42)}),
	$node->safe_psql(
		'postgres',
		q{SELECT doltgres_test_analyze('source_many', false, 100, 42)}),
	"ANALYZE's sample of the fork's table is heap's of the same ctids");

# ANALYZE samples what heap's acquire_sample_rows does: committed rows live,
# and the rows its own transaction inserts live. A rolled-back insert's rows
# are counted dead by heap until VACUUM, and by the fork as its spent places,
# the tuple ids the abort's take-back left for VACUUM to free (bugs row 96).

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_kept (a int4, b text);
INSERT INTO source_kept SELECT n, 'k' || n FROM generate_series(1, 20) n;
CREATE TABLE source_undone (a int4, b text);
INSERT INTO source_undone SELECT n, 'u' || n FROM generate_series(21, 50) n;
CREATE TABLE undone (a int4 PRIMARY KEY, b text);
SELECT doltgres_test_insert('undone', 'source_kept', false, 0);
BEGIN;
SELECT doltgres_test_insert('undone', 'source_undone', false, 0);
ROLLBACK;
CREATE TABLE heap_undone (a int4, b text);
INSERT INTO heap_undone SELECT * FROM source_kept;
BEGIN;
INSERT INTO heap_undone SELECT * FROM source_undone;
ROLLBACK;
});
my $undone = $node->safe_psql('postgres', q{SELECT doltgres_test_analyze('undone', true, 100, 42)});
is($undone,
	$node->safe_psql('postgres', q{SELECT doltgres_test_analyze('heap_undone', false, 100, 42)}),
	"ANALYZE samples and counts a table with a rolled-back insert as heap's does, its rolled-back rows dead");
like($undone, qr/live 20 dead 30$/, "ANALYZE counts a rolled-back insert's 30 rows dead until VACUUM");
my $own = q{
BEGIN;
CREATE TEMP TABLE own_tids AS
  SELECT doltgres_test_insert('undone', 'source_undone', false, 0);
SELECT doltgres_test_analyze('undone', true, 100, 42);
ROLLBACK;
};
is( $node->safe_psql('postgres', $own),
	$node->safe_psql(
		'postgres', q{
BEGIN;
INSERT INTO heap_undone SELECT * FROM source_undone;
SELECT doltgres_test_analyze('heap_undone', false, 100, 42);
ROLLBACK;
}),
	"ANALYZE samples and counts its own transaction's inserts live as heap's does, with the earlier rolled-back rows dead"
);
$node->safe_psql('postgres', "SELECT doltgres_test_vacuum('undone', 'auto'); VACUUM (INDEX_CLEANUP auto) heap_undone");
like(
	$node->safe_psql('postgres', q{SELECT doltgres_test_analyze('undone', true, 100, 42)}),
	qr/live 20 dead 0$/,
	"ANALYZE counts no rolled-back row dead once VACUUM freed their places");
is( $node->safe_psql('postgres', q{SELECT doltgres_test_analyze('undone', true, 100, 42)}),
	$node->safe_psql('postgres', q{SELECT doltgres_test_analyze('heap_undone', false, 100, 42)}),
	"ANALYZE's counts after VACUUM are heap's");

# Bugs row 93's count: heap's ANALYZE counts a deleted row's tuple dead until
# VACUUM frees it. A folded row deleted and the delete folded is a pending
# tuple id in the fork's base, which ANALYZE counts dead as one of the block's
# spent places until VACUUM frees it, so the sample and both counts equal heap's
# twin's before VACUUM and after it.
for my $table ('ended', 'heap_ended')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	$node->safe_psql('postgres', "CREATE TABLE $table (a int4, b text)");
	for my $statement (
		"INSERT INTO $table SELECT n, ''e'' || n FROM generate_series(1, 20) n",
		"DELETE FROM $table WHERE a <= 5")
	{
		$node->safe_psql('postgres',
			"SELECT doltgres_test_write('$table', $fork, '$statement')");
		$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')")
		  if $fork eq 'true';
	}
}
my $ended = sub {
	return [
		map {
			$node->safe_psql('postgres',
					"SELECT doltgres_test_analyze('$_', "
				  . ($_ =~ /^heap_/ ? 'false' : 'true')
				  . ', 100, 42)')
		} ('ended', 'heap_ended')
	];
};
my $before_vacuum = $ended->();
is($before_vacuum->[0], $before_vacuum->[1],
	"ANALYZE counts a folded row's deleted tuple dead as heap's twin does before VACUUM"
);
like(
	$before_vacuum->[1],
	qr/live 15 dead 5$/,
	"heap's ANALYZE counts the five deleted rows dead before VACUUM");
$node->safe_psql('postgres', "SELECT doltgres_test_vacuum('ended', 'auto')");
$node->safe_psql('postgres', 'VACUUM heap_ended');
my $after_vacuum = $ended->();
is($after_vacuum->[0], $after_vacuum->[1],
	"ANALYZE counts no dead tuple after VACUUM frees them, as heap's twin does"
);
like(
	$after_vacuum->[1],
	qr/live 15 dead 0$/,
	"heap's ANALYZE counts none dead after VACUUM");

# ANALYZE sorts a full sample by its rows' ctids, which a heap tuple copied from
# the fork's slot carries as a copy of heap's carries its tuple's, so a table
# with more rows than its sample analyzes as its heap twin does.
$node->safe_psql(
	'postgres', q{
CREATE TABLE sampled (a int4) USING heap;
CREATE TABLE heap_sampled (a int4);
ALTER TABLE sampled ALTER a SET STATISTICS 1;
ALTER TABLE heap_sampled ALTER a SET STATISTICS 1;
INSERT INTO sampled SELECT n FROM generate_series(1, 1000) n;
INSERT INTO heap_sampled SELECT n FROM generate_series(1, 1000) n;
ANALYZE sampled;
ANALYZE heap_sampled;
});
my $analyzed =
  q{SELECT c.reltuples || ' ' || s.n_distinct FROM pg_class c JOIN pg_stats s ON s.tablename = c.relname WHERE c.relname = '%s'};
is( $node->safe_psql('postgres', sprintf($analyzed, 'sampled')),
	$node->safe_psql('postgres', sprintf($analyzed, 'heap_sampled')),
	"ANALYZE of a table with more rows than its sample gives its heap twin's statistics"
);

$node->stop;
done_testing();
