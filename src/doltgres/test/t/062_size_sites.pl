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

# The sizes of the fork's relations and repositories (design item 5's
# "pg_relation_size and the database's size" and "Sizes for costs"; bugs row
# 132): pg_relation_size of a fork table is its block count times BLCKSZ and 0
# for its other forks, a fork index's is its tree's derived blocks with nbtree's
# metapage, and every size function agrees with them; pg_database_size and
# pg_tablespace_size add the repositories' files; and the fork's VACUUM writes
# the table's statistics, reports to the cumulative statistics, and warns of a
# temporary table's parallel option, as heap's does on the reference.

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
$node->init(allows_streaming => 1);
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');

# A fork table of a thousand uniform rows fills the pages heap's do on the
# reference, and an index on it, the metapage and its leaves and levels above
# them; an index with no entries is its metapage alone, as nbtree's is.
$node->safe_psql(
	'postgres', q{
CREATE TABLE sized (k int4, v text);
INSERT INTO sized SELECT g, repeat('x', 20) FROM generate_series(1, 1000) g;
CREATE INDEX sized_k ON sized (k);
CREATE INDEX sized_none ON sized (v) WHERE false;
});
is( $node->safe_psql(
		'postgres',
		q{SELECT pg_relation_size('sized'), pg_relation_size('sized_none')}),
	'65536|8192',
	'a fork table of a thousand rows is the eight pages heap fills, and an empty index its metapage'
);
is( $node->safe_psql(
		'postgres', q{
SELECT pg_relation_size('sized', 'fsm'), pg_relation_size('sized', 'vm'), pg_relation_size('sized', 'init')}),
	'0|0|0',
	'a fork table\'s other forks have no size');
is( $node->safe_psql(
		'postgres', q{
SELECT pg_relation_size('sized_k') % 8192 = 0 AND pg_relation_size('sized_k') >= 16384,
	pg_indexes_size('sized') = pg_relation_size('sized_k') + pg_relation_size('sized_none'),
	pg_table_size('sized') = pg_relation_size('sized')
		+ (SELECT pg_relation_size(reltoastrelid) + pg_indexes_size(reltoastrelid) FROM pg_class WHERE oid = 'sized'::regclass),
	pg_total_relation_size('sized') = pg_table_size('sized') + pg_indexes_size('sized')}),
	't|t|t|t',
	'a fork index is whole pages, its metapage and at least one leaf, and every size function sums its relations');

# The fork's VACUUM writes the table's relpages and exact reltuples, its
# relallvisible and relallfrozen equal to relpages (Differences row 10), and
# counts a vacuum, as heap's VACUUM does on the reference.
$node->safe_psql('postgres', 'VACUUM sized');
is( $node->safe_psql(
		'postgres', q{
SELECT relpages, reltuples, relallvisible = relpages, relallfrozen = relpages,
	relpages * 8192 = pg_relation_size('sized') FROM pg_class WHERE relname = 'sized'}),
	'8|1000|t|t|t',
	'VACUUM writes a fork table\'s pages and rows to pg_class');
is( $node->safe_psql(
		'postgres', q{
SELECT pg_stat_force_next_flush();
SELECT pg_stat_get_vacuum_count('sized'::regclass), pg_stat_get_last_vacuum_time('sized'::regclass) IS NOT NULL}),
	"\n1|t",
	'VACUUM counts a vacuum of a fork table in the cumulative statistics');

# A fork table's TOAST relation and a session's own temporary fork table are
# sized by the same count VACUUM writes as their relpages, which every size
# function then sums.
$node->safe_psql(
	'postgres', q{
CREATE TABLE toasted (k int4, v text);
ALTER TABLE toasted ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO toasted SELECT g, repeat(md5(g::text), 200) FROM generate_series(1, 50) g;
VACUUM toasted;
});
is( $node->safe_psql(
		'postgres', q{
SELECT pg_relation_size(t.oid) > 0, pg_relation_size(t.oid) = t.relpages::int8 * 8192,
	pg_table_size(c.oid) = pg_relation_size(c.oid) + pg_relation_size(t.oid) + pg_indexes_size(t.oid)
	FROM pg_class c JOIN pg_class t ON t.oid = c.reltoastrelid WHERE c.relname = 'toasted'}),
	't|t|t',
	'a fork table\'s TOAST relation is the pages VACUUM counts, and its table\'s size sums it');
is( $node->safe_psql(
		'postgres', q{
CREATE TEMPORARY TABLE temporary_sized (k int4, v text);
CREATE INDEX temporary_sized_k ON temporary_sized (k);
INSERT INTO temporary_sized SELECT g, repeat('x', 20) FROM generate_series(1, 1000) g;
VACUUM temporary_sized;
SELECT pg_relation_size('temporary_sized'), relpages::int8 * 8192 = pg_relation_size('temporary_sized'),
	pg_relation_size('temporary_sized_k') >= 16384,
	pg_total_relation_size('temporary_sized') = pg_table_size('temporary_sized') + pg_indexes_size('temporary_sized')
	FROM pg_class WHERE relname = 'temporary_sized'}),
	'65536|t|t|t',
	'a session\'s own temporary fork table and its index are sized as a permanent one\'s');

# VACUUM (PARALLEL 1) of a temporary table with two indexes warns that it
# disables the parallel option, as heap's does on the reference.
my ($result, $stdout, $stderr) = $node->psql(
	'postgres', q{
CREATE TEMPORARY TABLE tmp (a int4 PRIMARY KEY);
CREATE INDEX tmp_a ON tmp (a);
VACUUM (PARALLEL 1) tmp;
});
like(
	$stderr,
	qr/WARNING:  disabling parallel option of vacuum on "tmp" --- cannot vacuum temporary tables in parallel/,
	'VACUUM warns that a temporary fork table is never vacuumed in parallel');

# pg_database_size is the database's files in its directory, as Postgres
# counts them, and its repository's files through its subdirectories, and
# pg_tablespace_size adds every repository's files to Postgres' count of the
# default tablespace, so a checkpoint's packs count.
$node->safe_psql('postgres', 'CHECKPOINT');

# files returns SQL summing the sizes of a directory's entries, as dbsize.c's
# db_dir_size does.
my $files = sub {
	my ($dir) = @_;
	return
	  "coalesce((SELECT sum((pg_stat_file($dir || '/' || f)).size) FROM pg_ls_dir($dir) AS f), 0)";
};

# tree returns SQL summing the sizes of the files under a directory, through
# its subdirectories.
my $tree = sub {
	my ($dir) = @_;
	return qq{(WITH RECURSIVE tree(path) AS (SELECT $dir UNION ALL SELECT t.path || '/' || f FROM tree AS t,
		LATERAL pg_ls_dir(CASE WHEN (pg_stat_file(t.path, true)).isdir THEN t.path END) AS f)
		SELECT coalesce(sum((pg_stat_file(path, true)).size) FILTER (WHERE NOT (pg_stat_file(path, true)).isdir), 0)
		FROM tree)};
};
my $repository = $tree->(q{'pg_versioned/' || d.oid});
is( $node->safe_psql(
		'postgres', qq{
SELECT $repository > 0, pg_database_size(d.oid) = @{[ $files->(q{'base/' || d.oid}) ]} + $repository
	FROM pg_database d WHERE d.datname = current_database()}),
	't|t',
	'pg_database_size is the database directory\'s files and its repository\'s');
is( $node->safe_psql(
		'postgres', qq{
SELECT pg_tablespace_size('pg_default')
	- (SELECT sum((pg_stat_file('base/' || d)).size) FROM pg_ls_dir('base') AS d)
	- (SELECT sum(@{[ $files->(q{'base/' || d}) ]}) FROM pg_ls_dir('base') AS d)
	= @{[ $tree->(q{'pg_versioned'}) ]}}),
	't',
	'pg_tablespace_size adds every repository\'s files to the default tablespace\'s');

# A table's totals move at each commit by what the commit's rows change
# (bugs row 132): an aborted insert and a rolled-back subtransaction move
# nothing, and the totals after commits of inserts, updates, and deletes, on a
# keyed and a keyless table, equal a fresh count of the tree and its committed
# versions, as they do after a fold and after a crash's restart counts again.
sub totals
{
	return $node->safe_psql('postgres', "SELECT doltgres_test_totals('$_[0]')");
}

sub count
{
	return $node->safe_psql('postgres', "SELECT doltgres_test_count('$_[0]')");
}
$node->safe_psql(
	'postgres', q{
CREATE TABLE counted (k int4 PRIMARY KEY, v text);
CREATE TABLE counted_keyless (k int4, v text);
INSERT INTO counted SELECT g, 'v' || g FROM generate_series(1, 10) g;
INSERT INTO counted_keyless SELECT g % 3, 'v' FROM generate_series(1, 10) g;
});
my $before = totals('counted');
$node->safe_psql('postgres',
	q{BEGIN; INSERT INTO counted VALUES (11, 'v11'); ROLLBACK});
is(totals('counted'), $before, 'an aborted insert leaves a fork table\'s totals as they were');
$node->safe_psql('postgres',
	q{BEGIN; INSERT INTO counted VALUES (11, 'v11'); SAVEPOINT s; INSERT INTO counted VALUES (12, 'v12');
	ROLLBACK TO s; COMMIT});
is(totals('counted') =~ s/^(\S+ \d+) .* (\d+)$/$1 $2/r,
	'true 11 11', 'a commit after a rolled-back subtransaction counts its kept row alone');
is(totals('counted'), count('counted'),
	'an aborted insert and a rolled-back subtransaction leave a fork table\'s totals as they were');
$node->safe_psql(
	'postgres', q{
UPDATE counted SET v = repeat('w', 40) WHERE k <= 5;
DELETE FROM counted WHERE k > 8;
UPDATE counted_keyless SET v = 'w' WHERE k = 1;
DELETE FROM counted_keyless WHERE k = 2;
INSERT INTO counted_keyless VALUES (0, 'v'), (0, 'v');
});
is(totals('counted') . ' / ' . totals('counted_keyless'),
	count('counted') . ' / ' . count('counted_keyless'),
	'the totals after commits of inserts, updates, and deletes equal a fresh count, keyed and keyless');
$node->safe_psql('postgres',
	q{SELECT doltgres_test_fold('counted'); SELECT doltgres_test_fold('counted_keyless')});
is(totals('counted') . ' / ' . totals('counted_keyless'),
	count('counted') . ' / ' . count('counted_keyless'),
	'a fold after commits leaves the totals equal to a fresh count');
$node->safe_psql('postgres',
	q{INSERT INTO counted VALUES (20, 'v20'); UPDATE counted SET v = 'x' WHERE k = 1});
my $fresh = count('counted');
$node->stop('immediate');
$node->start;
like(totals('counted'), qr/^false /, 'a restart leaves the replayed table\'s totals unknown');
$node->safe_psql('postgres', q{SELECT pg_relation_size('counted')});
is(totals('counted'), $fresh,
	'a reader counts them again, to the count of the tree and the replayed versions');

# A TOAST relation's pages derive from its value map's totals: each value's
# chunks as heap stores them, TOAST_MAX_CHUNK_SIZE bytes each, and as many
# values to a page as fit whole, as heap writes a value's chunks one after
# another, which for values of one size is the reference's count.
my %shapes = (toast_full => [ 50, 200, 50 ], toast_large => [ 50, 80, 17 ], toast_small => [ 300, 66, 100 ]);
foreach my $name (sort keys %shapes)
{
	my ($rows, $repeats, $pages) = @{ $shapes{$name} };
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $name (k int4, v text);
ALTER TABLE $name ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO $name SELECT g, repeat(md5(g::text), $repeats) FROM generate_series(1, $rows) g;
});
	is( $node->safe_psql(
			'postgres',
			"SELECT pg_relation_size(reltoastrelid) / 8192 FROM pg_class WHERE relname = '$name'"),
		$pages,
		"a TOAST relation of $rows values of " . 32 * $repeats . " bytes is $pages pages");
}

# A TOAST relation's totals move at each commit by the values it writes and
# frees, as a table's do by its rows: an aborted insert moves nothing, an update
# adds its new values and keeps those it replaces and those a delete ends while
# row versions name them, as heap's TOAST relation keeps their chunks until
# VACUUM; values of mixed sizes count 60 pages where heap packs them in 55
# (Differences row 55); and the totals equal a fresh count after commits, a
# fold, and a crash's restart.
my $toast = $node->safe_psql('postgres',
	q{SELECT reltoastrelid::regclass FROM pg_class WHERE relname = 'toast_full'});
my $values = totals($toast);
$node->safe_psql('postgres',
	q{BEGIN; INSERT INTO toast_full VALUES (0, repeat('x', 9000)); ROLLBACK});
is(totals($toast), $values, 'an aborted insert leaves a TOAST relation\'s totals as they were');
$node->safe_psql(
	'postgres', q{
UPDATE toast_full SET v = repeat('y', 3000) WHERE k <= 10;
DELETE FROM toast_full WHERE k > 40;
});
is( totals($toast) =~ s/^(\S+ \d+) .*/$1/r
	  . ' / '
	  . $node->safe_psql('postgres', qq{SELECT pg_relation_size('$toast') / 8192}),
	'true 60 / 60',
	'an update adds its values and keeps those it replaces while row versions name them, as heap\'s pages do');
is(totals($toast), count($toast), 'the TOAST relation\'s totals after commits equal a fresh count');
$node->safe_psql('postgres', qq{SELECT doltgres_test_fold('$toast')});
is(totals($toast), count($toast), 'a fold of the TOAST relation leaves its totals equal to a fresh count');
$node->safe_psql('postgres', q{INSERT INTO toast_full VALUES (51, repeat('z', 5000))});
my $counted = count($toast);
$node->stop('immediate');
$node->start;
$node->safe_psql('postgres', qq{SELECT pg_relation_size('$toast')});
is(totals($toast), $counted, 'a restart counts a TOAST relation\'s totals again');

# A commit moves no totals for its rolled-back subtransaction's updates, keyed
# or keyless, and a rewrite, which drops the transient table it built its new
# storages in, leaves the new table's totals unknown until a reader counts them.
$node->safe_psql(
	'postgres', q{
BEGIN;
SAVEPOINT s;
UPDATE counted SET v = 'r' WHERE k = 2;
UPDATE counted_keyless SET v = 'r' WHERE k = 0;
ROLLBACK TO s;
UPDATE counted SET v = 's' WHERE k = 2;
UPDATE counted_keyless SET v = 's' WHERE k = 0;
COMMIT;
});
is(totals('counted') . ' / ' . totals('counted_keyless'),
	count('counted') . ' / ' . count('counted_keyless'),
	'a commit after a rolled-back subtransaction\'s updates counts its own updates alone, keyed and keyless');
$node->safe_psql('postgres', q{ALTER TABLE toast_full ALTER COLUMN k TYPE int8});
$toast = $node->safe_psql('postgres',
	q{SELECT reltoastrelid::regclass FROM pg_class WHERE relname = 'toast_full'});
like(totals('toast_full'), qr/^false /, 'a rewrite leaves its new table\'s totals unknown');
$node->safe_psql('postgres', q{SELECT pg_relation_size('toast_full')});
is(totals('toast_full') . ' / ' . totals($toast),
	count('toast_full') . ' / ' . count($toast),
	'a reader counts a rewrite\'s new table, and its TOAST relation has its fresh count as totals');

# A commit adds the moves of at most 64 storages in its critical section and
# marks the rest's totals unknown, which a reader counts again; and a backend
# terminated while it counts gives the count back at its exit, so the next
# reader counts the totals.
$node->safe_psql('postgres',
	join('', map { "CREATE TABLE many_$_ (k int4);" } 1 .. 70));
$node->safe_psql('postgres',
	'BEGIN;' . join('', map { "INSERT INTO many_$_ VALUES ($_);" } 1 .. 70) . 'COMMIT');
is( $node->safe_psql(
		'postgres', q{
SELECT count(*) FILTER (WHERE doltgres_test_totals(c.oid) LIKE 'false %'),
	count(*) FILTER (WHERE doltgres_test_totals(c.oid) = doltgres_test_count(c.oid))
	FROM pg_class c WHERE relname LIKE 'many\_%'}),
	'6|64',
	'a commit moves the totals of 64 storages and leaves the other 6\'s unknown');
is( $node->safe_psql(
		'postgres', q{
WITH sized AS MATERIALIZED (SELECT c.oid, pg_relation_size(c.oid) FROM pg_class c WHERE relname LIKE 'many\_%')
SELECT count(*) FROM sized WHERE doltgres_test_totals(oid) = doltgres_test_count(oid)}),
	'70',
	'a reader counts the totals a commit left unknown');
$node->safe_psql('postgres', q{INSERT INTO counted VALUES (21, 'v21')});
$node->stop('immediate');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-state-recounting', 'wait')");
my $recounting = $node->background_psql('postgres', on_error_stop => 0);
my $pid = $recounting->query_safe('SELECT pg_backend_pid()');
$recounting->query_until(qr/start/, "\\echo start\nSELECT pg_relation_size('counted');\n");
$node->wait_for_event('client backend', 'doltgres-state-recounting');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-state-recounting')");
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
ok( $node->poll_query_until(
		'postgres', "SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid"),
	'the counting backend is terminated');
$recounting->quit;
$node->safe_psql('postgres', q{SELECT pg_relation_size('counted')});
is(totals('counted'), count('counted'),
	'a backend terminated in a count gives it back at its exit, so the next reader counts the totals');

# A commit after a fold of its table, whose fold took the rows' writers' status
# entries while their values' map versions stayed, moves the TOAST relation's
# totals, which a reader counts to a fresh count.
$node->safe_psql(
	'postgres', q{
CREATE TABLE refolded (k int4 PRIMARY KEY, v text);
ALTER TABLE refolded ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO refolded VALUES (1, repeat('r', 3000));
SELECT doltgres_test_fold('refolded');
UPDATE refolded SET v = repeat('s', 3000);
});
my $refolded = $node->safe_psql('postgres',
	q{SELECT reltoastrelid::regclass FROM pg_class WHERE relname = 'refolded'});
$node->safe_psql('postgres', qq{SELECT pg_relation_size('$refolded')});
is(totals($refolded), count($refolded),
	'a commit after its table\'s fold moves its TOAST relation\'s totals to a fresh count');

# A VACUUM FULL of a toasted table builds its new TOAST relation's value map
# with the values' sizes, so the relation keeps the 8,192 bytes heap's has on
# the reference before and after, and its totals equal a fresh count.
$node->safe_psql(
	'postgres', q{
CREATE TABLE rebuilt (k int4 PRIMARY KEY, v text);
ALTER TABLE rebuilt ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO rebuilt SELECT g, repeat(chr(64 + g), 3000) FROM generate_series(1, 2) g;
SELECT doltgres_test_fold('rebuilt');
VACUUM FULL rebuilt;
});
my $rebuilt = $node->safe_psql('postgres',
	q{SELECT reltoastrelid::regclass FROM pg_class WHERE relname = 'rebuilt'});
is($node->safe_psql('postgres', qq{SELECT pg_relation_size('$rebuilt')}),
	'8192', 'a VACUUM FULL\'s TOAST relation keeps heap\'s size');
is(totals($rebuilt), count($rebuilt), 'a VACUUM FULL\'s TOAST relation\'s totals equal a fresh count');

# A hot standby's sizes equal its primary's, as heap's do (bugs row 132): its
# readers count the totals of a table and its TOAST relation over the rows
# written before its base backup and the rows it replays, after the replay,
# after its restart, and after its promote.
sub sized
{
	my ($on, $table) = @_;
	return $on->safe_psql(
		'postgres', qq{
SELECT pg_relation_size('$table'), pg_relation_size(reltoastrelid),
	doltgres_test_totals('$table'), doltgres_test_totals(reltoastrelid::regclass)
FROM pg_class WHERE relname = '$table'
});
}
$node->safe_psql(
	'postgres', q{
CREATE TABLE stood (k int4 PRIMARY KEY, v text);
ALTER TABLE stood ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO stood SELECT g, repeat(chr(64 + g % 26), 3000) FROM generate_series(1, 20) g;
});
$node->backup('sizes_backup');
my $standby = PostgreSQL::Test::Cluster->new('sizes_standby');
$standby->init_from_backup($node, 'sizes_backup', has_streaming => 1);
$standby->start;
$node->safe_psql(
	'postgres', q{
INSERT INTO stood SELECT g, repeat('n', 3000) FROM generate_series(21, 40) g;
UPDATE stood SET v = repeat('u', 3000) WHERE k <= 5;
DELETE FROM stood WHERE k > 35;
});
$node->wait_for_replay_catchup($standby);
my $primary_sizes = sized($node, 'stood');
is(sized($standby, 'stood'), $primary_sizes, 'a hot standby\'s sizes equal its primary\'s after the replay');
$standby->restart;
is(sized($standby, 'stood'), $primary_sizes, 'a hot standby\'s sizes equal its primary\'s after its restart');
$standby->promote;
is(sized($standby, 'stood'), $primary_sizes, 'a promoted standby\'s sizes equal its primary\'s');
$standby->stop;

$node->stop;
done_testing();
