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

# An index scan's cost against a sequential scan's on a server holding many
# databases' deltas: an entry whose fields are all inline finds no delta
# (bugs row 357), and an index scan over out-of-line fields keeps the delta
# its first read found for its whole life (bugs row 358), so a bitmap scan of
# inline entries and an index scan's reads of out-of-line values cost about
# what a sequential scan of the same rows costs, where a find for every entry
# swept the delta pool's every slot. Each bound sits between the measured
# ratios of the index read's time to the sequential scan's, before its fix and
# after: for the inline bitmap scan, 66.2 before and 4.18 after on Windows and
# 71.4 and 4.71 on the Mac, a bound of 17.7, the geometric mean of the least
# before and the greatest after; for the out-of-line index scan, 1.242 to
# 1.252 before and 0.814 to 0.817 after on Windows and 1.433 to 1.572 before
# and 1.026 to 1.132 after on the Mac, a bound of 1.35 that guards its cost
# with room for a loaded machine, since the last check, of the deltas a cursor
# holds, is the one that fails before its fix on both. A cursor's index scan
# reads its snapshot's rows across a write and a fold between its fetches, and
# gives its delta up at its close and at an abort.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $databases = 150;
my $rows = 4000;
my $width = 250;
my $bytes = 32 * $width;

# The server, with the tables the scans read, one whose fields are all
# inline and one whose indexed text is too wide to stay in the entry, and its
# other databases, each written to, so the server holds each one's delta.
# The cluster's locale is C on every machine, and the wide text's collation
# is ICU's root collation, so its index is ordered by a comparator whose reads
# resolve the entries' out-of-line keys, where the C locale's byte order
# would read none.
my $node = PostgreSQL::Test::Cluster->new('pooled');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf',
	"fold_threshold = 1GB\nfold_timeout = 3600s");
$node->start;
$node->safe_psql(
	'postgres', qq{
CREATE TABLE inline_rows (id int4 PRIMARY KEY, v int4, name name);
INSERT INTO inline_rows SELECT g, g % 1000, 'n' || g FROM generate_series(1, $rows) g;
CREATE INDEX inline_rows_v ON inline_rows (v);
CREATE TABLE outside_rows (id int4 PRIMARY KEY, v text COLLATE "und-x-icu");
INSERT INTO outside_rows SELECT g, (SELECT string_agg(md5(g::text || '.' || k::text), '') FROM generate_series(1, $width) k) FROM generate_series(1, $rows) g;
CREATE INDEX outside_rows_v ON outside_rows (v);
CREATE EXTENSION doltgres_test;
ANALYZE inline_rows, outside_rows});
for my $k (1 .. $databases)
{
	$node->safe_psql('postgres', "CREATE DATABASE other_$k");
	$node->safe_psql("other_$k",
		'CREATE TABLE written (a int4); INSERT INTO written VALUES (1)');
}

# An unfolded write of another table's out-of-line value, as wide as
# outside_rows' values, leaves the scans' database's delta holding its chunks,
# so the scans' reads find a delta.
$node->safe_psql(
	'postgres', q{
CREATE TABLE written_value (v text);
ALTER TABLE written_value ALTER COLUMN v SET STORAGE EXTERNAL;
});
my $writer = $node->safe_psql('postgres',
	"BEGIN; INSERT INTO written_value SELECT string_agg(md5(k::text), '') FROM generate_series(1, $width) k; SELECT txid_current(); COMMIT;"
);
cmp_ok($node->safe_psql('postgres', "SELECT doltgres_test_chunks($writer)"),
	'>', 0, "the scans' database's delta holds the written value's chunks");

# timed returns the least of several interleaved runs' execution times, in
# milliseconds, of a query read through an index, by the plan the settings
# choose, and read by a sequential scan, after asserting the index's plan.
sub timed
{
	my ($query, $settings, $plan) = @_;
	my $sequential =
	  'SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off;';
	my ($index, $seq);
	for (1 .. 5)
	{
		my $read = $node->safe_psql('postgres',
			"$settings EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) $query");
		like($read, $plan, "the query reads by $plan") if !defined $index;
		my ($ms) = $read =~ /Execution Time: ([0-9.]+) ms/;
		$index = $ms if !defined $index || $ms < $index;
		$read = $node->safe_psql('postgres',
			"$sequential EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) $query");
		($ms) = $read =~ /Execution Time: ([0-9.]+) ms/;
		$seq = $ms if !defined $seq || $ms < $seq;
	}
	return ($index, $seq);
}

my $bitmap =
  'SET enable_seqscan = off; SET enable_indexscan = off; SET enable_indexonlyscan = off;';
my $index =
  'SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off;';

# Bugs row 357's case: a bitmap scan over entries whose fields are all inline
# costs about what a sequential scan of the same rows costs.
my ($inline_index, $inline_seq) =
  timed('SELECT count(v) FROM inline_rows WHERE v >= 0',
	$bitmap, qr/Bitmap Index Scan on inline_rows_v/);
cmp_ok($inline_index, '<', 17.7 * $inline_seq,
	"a bitmap scan of inline entries costs about what a sequential scan does (row 357): $inline_index ms, then $inline_seq ms"
);

# Bugs row 358's cases: an index scan over out-of-line fields finds the delta
# once, so it costs about what a sequential scan of the same rows costs.
my ($index_index, $index_seq) =
  timed("SELECT sum(length(v)) FROM outside_rows WHERE v >= ''",
	$index, qr/Index Scan using outside_rows_v/);
cmp_ok($index_index, '<', 1.35 * $index_seq,
	"an index scan of out-of-line entries costs about what a sequential scan does (row 358): $index_index ms, then $index_seq ms"
);

# A cursor's index scan keeps its delta across fetches with an update of
# some rows and a fold of the table between them, and reads its snapshot's
# rows (row 358).
my $cursor = $node->background_psql('postgres');
$cursor->query_safe("$index BEGIN ISOLATION LEVEL REPEATABLE READ; "
	  . "DECLARE c CURSOR FOR SELECT length(v) FROM outside_rows WHERE v >= '' ORDER BY v"
);
my $first = $cursor->query_safe('FETCH 100 FROM c');
$node->safe_psql('postgres',
	"UPDATE outside_rows SET v = v || 'x' WHERE id <= 100; SELECT doltgres_test_fold('outside_rows')"
);
my $rest = $cursor->query_safe('FETCH ALL FROM c');
$cursor->query_safe('COMMIT');
$cursor->quit;
my @lengths = (split(/\n/, $first), split(/\n/, $rest));
is( scalar(@lengths) . '|' . (grep { $_ == $bytes } @lengths),
	"$rows|$rows",
	"a cursor reads its snapshot's rows across an update and a fold (row 358)"
);

# A cursor's index scan holds the delta while it reads and gives it up at
# its close and at its transaction's abort (row 358).
# One session's counts of its held deltas, one line each among its fetched
# rows' lengths: while its cursor reads, after its transaction's abort, and
# after its next cursor's close.
# The held cursor's query reads by the index, so its fetches read the rows'
# out-of-line fields through the index scan, and the delta still holds the
# written value's chunks for its first read to find.
my $held_query = "SELECT length(v) FROM outside_rows WHERE v >= '' ORDER BY v";
cmp_ok($node->safe_psql('postgres', "SELECT doltgres_test_chunks($writer)"),
	'>', 0,
	"the delta holds the written value's chunks before the held cursor reads (row 358)"
);
like($node->safe_psql('postgres', "$index EXPLAIN (COSTS OFF) $held_query"),
	qr/Index Scan using outside_rows_v/,
	"the held cursor's query reads by an index scan (row 358)");
my ($ret, $out, $err) = $node->psql(
	'postgres', qq{$index
BEGIN;
DECLARE held CURSOR FOR $held_query;
FETCH 10 FROM held;
SELECT doltgres_test_delta_held();
SELECT 1 / 0;
ROLLBACK;
SELECT doltgres_test_delta_held();
BEGIN;
DECLARE closed CURSOR FOR SELECT length(v) FROM outside_rows WHERE v >= '' ORDER BY v;
FETCH 10 FROM closed;
CLOSE closed;
SELECT doltgres_test_delta_held();
COMMIT;},
	on_error_stop => 0);
my @held = grep { $_ ne $bytes } split(/\n/, $out);
is( join(' ', @held),
	'1 0 0',
	"a cursor's index scan holds its database's delta and gives it up at an abort and at its close (row 358)"
);

$node->stop;
done_testing();
