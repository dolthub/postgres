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

# A streaming standby's replay of spilled batches (bugs row 298): a primary
# writes a committed batch of rows past its delta_memory, which it spills, and
# folds it while a scan of it is held at its read of the spill slots; writes
# two batches into another table, each folded while a point fetch of a spilled
# row is held the same way; and writes a second batch into the first table,
# which the standby replays, folds at its primary's point while its own scan is
# held, and reads whole. Before, the standby's redo of the second batch's row
# changes died with "a location of the delta names no record its hold
# reaches".

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $reading = PostgreSQL::Test::Cluster->new('spill_reading');
$reading->init(allows_streaming => 1);
$reading->append_conf('postgresql.conf',
	"fold_threshold = 1GB\nfold_timeout = 3600s\nmax_parallel_workers_per_gather = 0"
);
$reading->start;
$reading->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION doltgres_test;
CREATE TABLE read_source (v text) USING doltgres_twin;
INSERT INTO read_source SELECT 'r' || i FROM generate_series(1, 100000) i;
CREATE TABLE read_rows (v text);
});
$reading->backup('reading_backup');
my $reading_standby = PostgreSQL::Test::Cluster->new('spill_reading_standby');
$reading_standby->init_from_backup($reading, 'reading_backup',
	has_streaming => 1);
$reading_standby->append_conf('postgresql.conf',
	"max_parallel_workers_per_gather = 0");
$reading_standby->start;
my $read_count =
  q{SELECT count(*) FROM doltgres_test_scan('read_rows', false, -1)};

# held_scan starts a scan of read_rows on a node in a session of its own, held at
# the point where it reads the storage's spill slots, and returns the session.
sub held_scan
{
	my ($on) = @_;
	my $scan = $on->background_psql('postgres');
	$scan->query_safe(
		"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-spill-slots-read', 'wait')"
	);
	$scan->query_until(qr/held/, "\\echo held\n$read_count;\n");
	$on->wait_for_event('client backend', 'doltgres-spill-slots-read');
	return $scan;
}

# released lets a node's held scan go on and returns its count.
sub released
{
	my ($on, $scan) = @_;
	$on->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-spill-slots-read'); SELECT injection_points_wakeup('doltgres-spill-slots-read')"
	);
	my $count = $scan->query_safe('SELECT 1');
	$scan->quit;
	return $count;
}
$reading->safe_psql(
	'postgres',
	q{SELECT count(*) FROM doltgres_test_insert('read_rows', 'read_source', true, 0)},
	timeout => 1200);
$reading->wait_for_catchup($reading_standby);
my $scan = held_scan($reading);
$reading->safe_psql('postgres', q{SELECT doltgres_test_fold('read_rows')});
released($reading, $scan);
is($reading->safe_psql('postgres', $read_count),
	'100000', 'and a scan after it reads every row');

# A fetch of a spilled row by its tuple id and an index fetch of it by its row
# key, each held inside its read of the delta while a fold installs a base
# holding the row and drops its spill, then read again.
$reading->safe_psql(
	'postgres', q{
CREATE TABLE point_source USING doltgres_twin AS SELECT 'p' || i AS v FROM generate_series(1, 100000) i;
CREATE TABLE point_rows (v text);
CREATE TABLE point_one AS SELECT 'p1' AS v;
});
# copied writes the point table's batch of spilled rows and returns its first
# row's tuple id, which its first spill holds, and that id as a number.
sub copied
{
	my $tid = $reading->safe_psql(
		'postgres',
		q{SELECT min(t) FROM doltgres_test_insert('point_rows', 'point_source', true, 0) t},
		timeout => 1200);
	return (
		$tid,
		$reading->safe_psql(
			'postgres',
			"SELECT ((('$tid'::tid::text::point)[0])::int8 << 16) | (('$tid'::tid::text::point)[1])::int8"
		));
}
for my $name ('by tuple id', 'by row key')
{
	my ($tid, $copy) = copied();
	my $fetch =
	  $name eq 'by tuple id'
	  ? "SELECT doltgres_test_fetch('point_rows', '$tid')"
	  : "SELECT string_agg(r, ' ') FROM doltgres_test_index_fetch_key('point_rows', 'point_one', $copy) r";
	my $point = $reading->background_psql('postgres');
	$point->query_safe(
		"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-spill-slots-read', 'wait')"
	);
	$point->query_until(qr/held/, "\\echo held\n$fetch;\n");
	$reading->wait_for_event('client backend', 'doltgres-spill-slots-read');
	$reading->safe_psql('postgres',
		q{SELECT doltgres_test_fold('point_rows')});
	$reading->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-spill-slots-read'); SELECT injection_points_wakeup('doltgres-spill-slots-read')"
	);
	$point->query_safe('SELECT 1');
	$point->quit;
	isnt($reading->safe_psql('postgres', $fetch),
		'', "a fetch $name reads the row after the fold");
}
$reading->safe_psql(
	'postgres',
	q{SELECT count(*) FROM doltgres_test_insert('read_rows', 'read_source', true, 0)},
	timeout => 1200);
$reading->wait_for_catchup($reading_standby);
$scan = held_scan($reading_standby);
$reading->safe_psql('postgres', q{SELECT doltgres_test_fold('read_rows')});
# A standby folds a byte-ordered table's reached point only as the next record
# arrives (bugs row 334, whose fix takes this record out), so one follows the
# fold, and the standby's own fold runs until its base is its primary's, since
# one run after its catch-up isn't always enough (bugs row 337, whose fix takes
# the retry out).
$reading->safe_psql('postgres', 'SELECT txid_current()');
$reading->wait_for_catchup($reading_standby);
my $points = q{SELECT doltgres_test_base_sizes('read_rows')};
my $primary_points = $reading->safe_psql('postgres', $points);
ok( $reading_standby->poll_query_until(
		'postgres',
		"SELECT doltgres_test_base_sizes('read_rows') = '$primary_points' FROM (SELECT doltgres_test_fold('read_rows')) f"
	),
	"the standby folded at its primary's point while its scan was held");
released($reading_standby, $scan);
is($reading_standby->safe_psql('postgres', $read_count),
	'200000', "and the standby's scan after it reads every row");
$reading_standby->stop;
$reading->stop;

# Bugs row 339: one INSERT past a 1MB delta_memory finishes, a wide one and a
# narrow one, and the 1MB standby replays each; before, the primary's writer
# waited at the budget for good.
my $small = PostgreSQL::Test::Cluster->new('spill_small');
$small->init(allows_streaming => 1);
$small->append_conf('postgresql.conf',
	"delta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s");
$small->start;
$small->backup('small_backup');
my $small_standby = PostgreSQL::Test::Cluster->new('spill_small_standby');
$small_standby->init_from_backup($small, 'small_backup', has_streaming => 1);
$small_standby->start;

foreach my $case ([ 'wide_rows', 3000, q{repeat('w', 600)} ],
	[ 'narrow_rows', 20000, q{'n'} ])
{
	my ($table, $rows, $value) = @$case;
	my (undef, $out) = $small->psql(
		'postgres', qq{
SET statement_timeout = '180s';
CREATE TABLE $table (k int4 PRIMARY KEY, v text) WITH (fillfactor = 50);
INSERT INTO $table SELECT g, $value FROM generate_series(1, $rows) g;
SELECT count(*) FROM $table;
});
	is($out, $rows,
		"an INSERT of $rows rows into $table past a 1MB delta_memory finishes"
	);
	$small->wait_for_catchup($small_standby);
	is( $small_standby->safe_psql('postgres', "SELECT count(*) FROM $table"),
		$rows,
		"and the 1MB standby replays it");
}
$small_standby->stop;
$small->stop;

# Bugs row 332: a 1MB standby replays a VACUUM FULL whose built nodes pass its
# delta_memory, spilling them under their rewrite's xid as a blob's chunks
# spill; a rewrite aborted after its chunk records leaves the standby no
# listed spill file and no built node; and a standby restarted between a
# rewrite's chunk records and its base record installs its primary's base from
# the nodes it spilled. Before, the standby's startup waited at the budget for
# good. A transaction aborted after the standby spilled its out-of-line
# values' chunks leaves no listed spill file either, and one that spilled
# nothing leaves the manifest unwritten.
my $building = PostgreSQL::Test::Cluster->new('spill_building');
$building->init(allows_streaming => 1);
$building->append_conf('postgresql.conf',
	"delta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s");
$building->start;
$building->safe_psql('postgres',
	'CREATE EXTENSION injection_points; CREATE EXTENSION doltgres_test');
$building->backup('building_backup');
my $building_standby =
  PostgreSQL::Test::Cluster->new('spill_building_standby');
$building_standby->init_from_backup($building, 'building_backup',
	has_streaming => 1);
$building_standby->start;

foreach my $table ('built_whole', 'built_aborted', 'built_restarted')
{
	$building->safe_psql(
		'postgres', qq{
CREATE TABLE $table (k int4 PRIMARY KEY, v text) WITH (fillfactor = 50);
INSERT INTO $table SELECT g, repeat('w', 600) FROM generate_series(1, 6000) g;
SELECT doltgres_test_fold('$table');
});
}
$building->wait_for_catchup($building_standby);
my $built_rows = q{SELECT count(*), sum(length(v)) FROM %s};
my $built_spills =
  q{SELECT count(*) FROM regexp_split_to_table(coalesce(pg_read_file('pg_versioned/' || (SELECT oid FROM pg_database WHERE datname = 'postgres') || '/delta/spill/spills-chunks', true), ''), E'\\n') line WHERE split_part(line, ' ', 1) = '%s'};

$building->safe_psql('postgres', 'VACUUM FULL built_whole');
$building->wait_for_catchup($building_standby);
is( $building_standby->safe_psql(
		'postgres', sprintf($built_rows, 'built_whole')),
	'6000|3600000',
	"the 1MB standby replays a VACUUM FULL whose built nodes pass its delta_memory, and reads every row"
);

my $aborting = $building->background_psql('postgres', on_error_stop => 0);
$aborting->query_safe('SELECT injection_points_set_local()');
$aborting->query_safe(
	"SELECT injection_points_attach('doltgres-built-logged', 'wait')");
my $aborted = $aborting->query_safe('BEGIN; SELECT txid_current()');
$aborting->query_until(qr/start/,
	"\\echo start\nCLUSTER built_aborted USING built_aborted_pkey;\n");
$building->wait_for_event('client backend', 'doltgres-built-logged');
$building->wait_for_catchup($building_standby, 'replay',
	$building->lsn('insert'));
isnt(
	$building_standby->safe_psql(
		'postgres', sprintf($built_spills, $aborted)),
	'0',
	"the standby spills a running rewrite's built nodes into files its chunk spills' manifest lists under the rewrite's xid"
);
$building->safe_psql('postgres',
	"SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE wait_event = 'doltgres-built-logged'"
);
$aborting->query('ROLLBACK');
$aborting->quit;
$building->wait_for_catchup($building_standby, 'replay',
	$building->lsn('insert'));
is( $building_standby->safe_psql(
		'postgres', sprintf($built_spills, $aborted)),
	'0',
	"the rewrite's abort takes its files out of the standby's manifest");
is( $building_standby->safe_psql(
		'postgres', 'SELECT doltgres_test_built_nodes()'),
	'0',
	"nor any of its built nodes");
is( $building_standby->safe_psql(
		'postgres', sprintf($built_rows, 'built_aborted')),
	'6000|3600000',
	"and the standby reads the table's rows as before the rewrite");

my $restarting = $building->background_psql('postgres');
$restarting->query_safe('SELECT injection_points_set_local()');
$restarting->query_safe(
	"SELECT injection_points_attach('doltgres-built-logged', 'wait')");
$restarting->query_until(qr/start/,
	"\\echo start\nVACUUM FULL built_restarted;\n");
$building->wait_for_event('client backend', 'doltgres-built-logged');
my $restarted = $building->safe_psql('postgres',
	"SELECT backend_xid FROM pg_stat_activity WHERE wait_event = 'doltgres-built-logged'"
);
$building->safe_psql('postgres', 'CHECKPOINT');
$building->wait_for_catchup($building_standby);
$building_standby->safe_psql('postgres', 'CHECKPOINT');
isnt(
	$building_standby->safe_psql(
		'postgres', sprintf($built_spills, $restarted)),
	'0',
	"a standby's restartpoint past a running rewrite's chunk records keeps its spilled built nodes"
);
$building_standby->restart;
$building->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-built-logged')");
$restarting->quit;
$building->wait_for_catchup($building_standby);
is( $building_standby->safe_psql(
		'postgres', "SELECT doltgres_test_base_sizes('built_restarted')"),
	$building->safe_psql(
		'postgres', "SELECT doltgres_test_base_sizes('built_restarted')"),
	"the restarted standby installs its primary's built base from the nodes it spilled"
);
is( $building_standby->safe_psql(
		'postgres', sprintf($built_rows, 'built_restarted')),
	'6000|3600000',
	"and reads every row");

$building->safe_psql('postgres', 'CREATE TABLE toasted_aborted (v text)');
my $toasting = $building->background_psql('postgres');
my $toasted = $toasting->query_safe('BEGIN; SELECT txid_current()');
$toasting->query_safe(
	"INSERT INTO toasted_aborted SELECT repeat(md5((-i)::text), 250) FROM generate_series(1, 300) i"
);
$building->wait_for_catchup($building_standby, 'replay',
	$building->lsn('insert'));
isnt(
	$building_standby->safe_psql(
		'postgres', sprintf($built_spills, $toasted)),
	'0',
	"the standby spills a running transaction's out-of-line values' chunks under its xid"
);
$toasting->query_safe(
	"INSERT INTO toasted_aborted SELECT repeat(md5((-i)::text), 250) FROM generate_series(301, 303) i"
);
$building->wait_for_catchup($building_standby, 'replay',
	$building->lsn('insert'));
cmp_ok(
	$building_standby->safe_psql(
		'postgres', "SELECT doltgres_test_chunks($toasted)"),
	'>', 0,
	"and holds in its delta the chunks the transaction wrote since its last spill"
);
$toasting->query_safe('ROLLBACK');
$toasting->quit;
$building->wait_for_catchup($building_standby, 'replay',
	$building->lsn('insert'));
is( $building_standby->safe_psql(
		'postgres', sprintf($built_spills, $toasted)),
	'0',
	"and the transaction's abort takes them out of the standby's manifest");

# The replayed abort takes the transaction's chunks still in the delta out, so
# a later record's relief at the standby's full budget spills none of them
# into a file no sweep would take out (bugs row 332).
my $toasted_page = (8 << 32) | (1024 - 64);
my $toasted_full =
  $building_standby->safe_psql('postgres',
	'SELECT doltgres_test_shared(0, 0, 0)');
$building_standby->safe_psql('postgres',
	"SELECT doltgres_test_delta(20, $toasted_full, $toasted_page)");
$building->safe_psql('postgres',
	'CREATE TABLE toasted_later (v text); INSERT INTO toasted_later VALUES (1)'
);
my $toasted_later =
  $building->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$building_standby->poll_query_until('postgres',
	"SELECT pg_last_wal_replay_lsn() >= '$toasted_later'::pg_lsn OR (SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'startup' AND wait_event = 'versioned delta budget')"
);
$building_standby->safe_psql('postgres',
	"SELECT doltgres_test_delta(21, $toasted_full, 0)");
$building->wait_for_catchup($building_standby, 'replay',
	$building->lsn('insert'));
is( $building_standby->safe_psql(
		'postgres', sprintf($built_spills, $toasted)),
	'0',
	"a later record's relief at the full budget spills none of the aborted transaction's chunks"
);

$building->safe_psql('postgres',
	'CREATE TABLE toasted_kept (v text); INSERT INTO toasted_kept SELECT repeat(md5(i::text), 250) FROM generate_series(1, 300) i'
);
$building->wait_for_catchup($building_standby);
my $manifest_written =
  q{SELECT modification FROM pg_stat_file('pg_versioned/' || (SELECT oid FROM pg_database WHERE datname = 'postgres') || '/delta/spill/spills-chunks')};
my $written = $building_standby->safe_psql('postgres', $manifest_written);
sleep(2);
my $quiet = $building->background_psql('postgres');
$quiet->query_safe('BEGIN; SELECT txid_current()');
$quiet->query_safe("INSERT INTO toasted_aborted VALUES ('q')");
$quiet->query_safe('ROLLBACK');
$quiet->quit;
$building->wait_for_catchup($building_standby, 'replay',
	$building->lsn('insert'));
is( $building_standby->safe_psql('postgres', $manifest_written),
	$written,
	"a replayed abort of a transaction that spilled nothing leaves the standby's chunk spills' manifest unwritten"
);
$building_standby->stop;
$building->stop;

# A 1MB standby whose committed writers' free space map takes fill the rest
# of its delta, most of it held by a test chain, spills them through the
# budget's committed arm, as it spills their rows, so its replay finishes, and
# its free space entries are its primary's (bugs row 332).
my $mapping = PostgreSQL::Test::Cluster->new('spill_mapping');
$mapping->init(allows_streaming => 1);
$mapping->append_conf('postgresql.conf',
	"fold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off\ncheckpoint_timeout = 1h"
);
$mapping->start;
$mapping->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
$mapping->backup('mapping_backup');
my $mapping_standby = PostgreSQL::Test::Cluster->new('spill_mapping_standby');
$mapping_standby->init_from_backup($mapping, 'mapping_backup',
	has_streaming => 1);
$mapping_standby->append_conf('postgresql.conf',
	"delta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h"
);
$mapping_standby->start;
$mapping->safe_psql('postgres', 'CREATE TABLE mapped (k int4)');
$mapping->wait_for_catchup($mapping_standby);
my $held = $mapping_standby->safe_psql('postgres',
	'SELECT doltgres_test_shared(0, 0, 0)');
$mapping_standby->safe_psql('postgres',
	"SELECT doltgres_test_delta(29, $held, 80)");

for my $batch (0 .. 3)
{
	my $from = 3000 * $batch + 1;
	$mapping->safe_psql('postgres',
		"INSERT INTO mapped SELECT g FROM generate_series($from, $from + 2999) g"
	);
}
$mapping->wait_for_replay_catchup($mapping_standby);
my $mapped_entries =
  q{SELECT string_agg(b || ':' || doltgres_test_free_space('mapped', b), ' ' ORDER BY b) FROM generate_series(0, (pg_relation_size('mapped') / 8192 - 1)::int4) b};
is( $mapping_standby->safe_psql('postgres', $mapped_entries),
	$mapping->safe_psql('postgres', $mapped_entries),
	"a 1MB standby whose committed writers' free space takes filled its delta replays them all, its free space entries its primary's"
);
$mapping_standby->safe_psql('postgres',
	"SELECT doltgres_test_delta(21, $held, 0)");
$mapping_standby->stop;
$mapping->stop;

# Bugs row 332: a standby's replayed abort takes its transaction's blobs'
# chunks out of its delta, as its primary's abort does, for a rollback to a
# savepoint, a rollback to an outer savepoint whose released inner one wrote
# them, a ROLLBACK PREPARED, and a ROLLBACK PREPARED after a restart that gave
# the chunks back from the restartpoint's runs.
my $rolling = PostgreSQL::Test::Cluster->new('spill_rolling');
$rolling->init(allows_streaming => 1);
$rolling->append_conf('postgresql.conf',
	"max_prepared_transactions = 4\nfold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h"
);
$rolling->start;
$rolling->safe_psql('postgres',
	'CREATE EXTENSION doltgres_test; CREATE TABLE toasted_rolled (v text)');
$rolling->backup('rolling_backup');
my $rolling_standby = PostgreSQL::Test::Cluster->new('spill_rolling_standby');
$rolling_standby->init_from_backup($rolling, 'rolling_backup',
	has_streaming => 1);
$rolling_standby->start;
my $rolled_insert =
  'INSERT INTO toasted_rolled SELECT repeat(md5((-i)::text), 250) FROM generate_series(1, 20) i';
my $rolled_chunks = 'SELECT doltgres_test_chunks(%s)';

my %rolled_forms = (
	'a rollback to a savepoint' =>
	  [ 'SAVEPOINT s', $rolled_insert, 'ROLLBACK TO s', 'COMMIT' ],
	"a rollback to an outer savepoint whose released inner one wrote them" =>
	  [
		'SAVEPOINT o',
		'SAVEPOINT i',
		$rolled_insert,
		'RELEASE i',
		'ROLLBACK TO o',
		'COMMIT'
	  ],
	'a ROLLBACK PREPARED' => [
		$rolled_insert,
		"PREPARE TRANSACTION 'rolled'",
		"ROLLBACK PREPARED 'rolled'"
	]);

foreach my $form (sort keys %rolled_forms)
{
	my @steps = @{ $rolled_forms{$form} };
	my $ending = $steps[-1];
	my $session = $rolling->background_psql('postgres');
	my $top = $session->query_safe('BEGIN; SELECT txid_current()');

	$session->query_safe($_) foreach @steps[ 0 .. $#steps - 2 ];
	$rolling->wait_for_catchup($rolling_standby, 'replay',
		$rolling->lsn('insert'));
	cmp_ok(
		$rolling_standby->safe_psql(
			'postgres', sprintf($rolled_chunks, $top)),
		'>', 0,
		"before $form, the standby's delta holds the transaction's chunks");
	$session->query_safe($steps[-2]);
	$ending =~ /PREPARED/
	  ? $rolling->safe_psql('postgres', $ending)
	  : $session->query_safe($ending);
	$session->quit;
	$rolling->wait_for_catchup($rolling_standby);
	is( $rolling_standby->safe_psql(
			'postgres', sprintf($rolled_chunks, $top)),
		'0',
		"the standby's replay of $form takes them out of its delta");
}

my $restarted_session = $rolling->background_psql('postgres');
my $restarted_top =
  $restarted_session->query_safe('BEGIN; SELECT txid_current()');
$restarted_session->query_safe($rolled_insert);
$restarted_session->query_safe("PREPARE TRANSACTION 'restarted'");
$restarted_session->quit;
$rolling->safe_psql('postgres', 'CHECKPOINT');
$rolling->wait_for_catchup($rolling_standby);
$rolling_standby->safe_psql('postgres', 'CHECKPOINT');
$rolling_standby->restart;
cmp_ok(
	$rolling_standby->safe_psql(
		'postgres', sprintf($rolled_chunks, $restarted_top)),
	'>', 0,
	"a standby restarted from a restartpoint after a prepared transaction's chunks holds them again"
);
$rolling->safe_psql('postgres', "ROLLBACK PREPARED 'restarted'");
$rolling->wait_for_catchup($rolling_standby);
is( $rolling_standby->safe_psql(
		'postgres', sprintf($rolled_chunks, $restarted_top)),
	'0',
	"and its replay of the ROLLBACK PREPARED takes them out of its delta");
$rolling_standby->stop;
$rolling->stop;

# Bugs row 359: a writer a crash ends writes no abort record, so its blobs'
# chunks go when its standby replays a running transactions record that shows
# it ended uncommitted, or at the end of a recovery: a standby's promotion,
# which keeps the chunks of a transaction prepared after its last
# restartpoint, or a primary's own crash recovery. The pair's epoch is moved,
# so a writer's full xid isn't its 32-bit one, as the running transactions
# record holds it.
my $ending = PostgreSQL::Test::Cluster->new('spill_ending');
$ending->init(allows_streaming => 1);
$ending->append_conf('postgresql.conf',
	"max_prepared_transactions = 4\nfold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h"
);
command_ok([ 'pg_resetwal', '-e', '1', $ending->data_dir ],
	'the ending pair\'s epoch is moved');
$ending->start;
$ending->safe_psql('postgres',
	'CREATE EXTENSION doltgres_test; CREATE TABLE toasted_ended (v text)');
$ending->backup('ending_backup');
my $ending_standby = PostgreSQL::Test::Cluster->new('spill_ending_standby');
$ending_standby->init_from_backup($ending, 'ending_backup',
	has_streaming => 1);
$ending_standby->start;
my $ended_insert =
  'INSERT INTO toasted_ended SELECT repeat(md5((-i)::text), 250) FROM generate_series(1, 20) i';
my $ended_chunks = 'SELECT doltgres_test_chunks(%s)';
my $ended_open = sub {
	my $session = $ending->background_psql('postgres');
	my $top = $session->query_safe('BEGIN; SELECT txid_current()');
	$session->query_safe($ended_insert);
	return ($session, $top);
};

my ($crashed_session, $crashed) = $ended_open->();
cmp_ok($crashed, '>', 0xffffffff,
	"the crashed writer's full xid isn't its 32-bit one");
$ending->wait_for_catchup($ending_standby, 'replay', $ending->lsn('insert'));
cmp_ok(
	$ending_standby->safe_psql('postgres', sprintf($ended_chunks, $crashed)),
	'>', 0, "the standby's delta holds an open transaction's chunks");
$ending->stop('immediate');
eval { $crashed_session->quit };
$ending->start;
$ending->safe_psql('postgres', 'CHECKPOINT');
$ending->wait_for_catchup($ending_standby);
is( $ending_standby->safe_psql('postgres', sprintf($ended_chunks, $crashed)),
	'0',
	"after its primary's crash, the standby's replay of a running transactions record takes them out"
);

$ending->safe_psql('postgres', 'CHECKPOINT');
$ending->wait_for_catchup($ending_standby);
$ending_standby->safe_psql('postgres', 'CHECKPOINT');
my $prepared_session = $ending->background_psql('postgres');
my $prepared = $prepared_session->query_safe('BEGIN; SELECT txid_current()');
$prepared_session->query_safe($ended_insert);
$prepared_session->query_safe("PREPARE TRANSACTION 'ended'");
$prepared_session->quit;
my ($promoted_session, $promoted) = $ended_open->();
$ending->wait_for_catchup($ending_standby, 'replay', $ending->lsn('insert'));
cmp_ok(
	$ending_standby->safe_psql('postgres', sprintf($ended_chunks, $promoted)),
	'>',
	0,
	"before its promotion, the standby's delta holds an open transaction's chunks"
);
$ending_standby->promote;
is( $ending_standby->safe_psql('postgres', sprintf($ended_chunks, $promoted)),
	'0',
	"the standby's promotion takes them out");
$ending_standby->safe_psql('postgres', "COMMIT PREPARED 'ended'");
is( $ending_standby->safe_psql(
		'postgres', 'SELECT count(*), sum(length(v)) FROM toasted_ended'),
	'20|160000',
	"and keeps those of a transaction prepared after its last restartpoint, whose values read back whole after its COMMIT PREPARED"
);
$ending_standby->stop;
$promoted_session->quit;

my ($recovered_session, $recovered) = $ended_open->();
cmp_ok($ending->safe_psql('postgres', sprintf($ended_chunks, $recovered)),
	'>', 0, "the primary's delta holds an open transaction's chunks");
$ending->stop('immediate');
eval { $recovered_session->quit };
$ending->start;
is($ending->safe_psql('postgres', sprintf($ended_chunks, $recovered)),
	'0', "and its crash recovery takes them out");
$ending->safe_psql('postgres', "ROLLBACK PREPARED 'ended'");
$ending->stop;

# Bugs row 362: at the end of recovery, a prepared transaction's trees spill
# as a live prepare's relief spills its own, so a recovered prepared
# transaction holds what a live one holds: after a promotion or a primary's
# crash recovery its ROLLBACK PREPARED leaves nothing in the delta and, after a
# checkpoint, nothing listed under its xid, its COMMIT PREPARED's values read
# back whole, and a transaction whose replay already spilled part of its
# chunks composes with it. The pair's epoch is moved, and its standby's 1MB
# delta_memory makes replay spill the larger insert's chunks.
my $preparing = PostgreSQL::Test::Cluster->new('spill_preparing');
$preparing->init(allows_streaming => 1);
$preparing->append_conf('postgresql.conf',
	"max_prepared_transactions = 8\nfold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h"
);
command_ok([ 'pg_resetwal', '-e', '1', $preparing->data_dir ],
	'the preparing pair\'s epoch is moved');
$preparing->start;
$preparing->safe_psql('postgres',
	'CREATE EXTENSION doltgres_test; CREATE TABLE toasted_prepared (v text)');
$preparing->safe_psql('postgres', 'CREATE DATABASE preparing_other');
$preparing->safe_psql('preparing_other',
	'CREATE EXTENSION doltgres_test; CREATE TABLE toasted_prepared (v text)');
$preparing->backup('preparing_backup');
my $preparing_standby =
  PostgreSQL::Test::Cluster->new('spill_preparing_standby');
$preparing_standby->init_from_backup($preparing, 'preparing_backup',
	has_streaming => 1);
$preparing_standby->append_conf('postgresql.conf',
	"delta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h"
);
$preparing_standby->start;
my $prepared_chunks = 'SELECT doltgres_test_chunks(%s)';
my $prepared_held =
  q{SELECT doltgres_test_chunks(%1$s) + (SELECT count(*) FROM regexp_split_to_table(coalesce(pg_read_file('pg_versioned/' || (SELECT oid FROM pg_database WHERE datname = current_database()) || '/delta/spill/spills-chunks', true), ''), E'\\n') line WHERE split_part(line, ' ', 1) = '%1$s')};
my $prepare = sub {
	my ($on, $database, $name, $rows) = @_;
	my $session = $on->background_psql($database);
	my $top = $session->query_safe('BEGIN; SELECT txid_current()');
	$session->query_safe(
		"INSERT INTO toasted_prepared SELECT repeat(md5('$name' || i), 250) FROM generate_series(1, $rows) i"
	);
	$session->query_safe("PREPARE TRANSACTION '$name'");
	$session->quit;
	return $top;
};
my $prepared_small = $prepare->($preparing, 'preparing_other', 'small', 20);
my $prepared_large = $prepare->($preparing, 'postgres', 'large', 300);
my $prepared_kept = $prepare->($preparing, 'postgres', 'kept', 20);
cmp_ok($prepared_large, '>', 0xffffffff,
	"the prepared writers' full xids aren't their 32-bit ones");
$preparing->wait_for_catchup($preparing_standby, 'replay',
	$preparing->lsn('insert'));
cmp_ok(
	$preparing_standby->safe_psql(
		'preparing_other', sprintf($prepared_held, $prepared_small)),
	'>', 0,
	"before its promotion, the standby holds a prepared transaction's chunks in a second database"
);
cmp_ok(
	$preparing_standby->safe_psql(
		'postgres', sprintf($built_spills, $prepared_large)),
	'>', 0,
	"and its replay spilled part of a larger prepared transaction's chunks");
$preparing_standby->promote;
is( $preparing_standby->safe_psql('preparing_other',
		sprintf($prepared_chunks, $prepared_small))
	  . $preparing_standby->safe_psql('postgres',
		sprintf($prepared_chunks, $prepared_large))
	  . $preparing_standby->safe_psql(
		'postgres', sprintf($prepared_chunks, $prepared_kept)),
	'000',
	"the promotion spills the prepared transactions' chunks out of its delta, as a live prepare spills them"
);
$preparing_standby->safe_psql('preparing_other', "ROLLBACK PREPARED 'small'");
$preparing_standby->safe_psql('postgres',
	"ROLLBACK PREPARED 'large'; COMMIT PREPARED 'kept'; CHECKPOINT");
$preparing_standby->safe_psql('preparing_other', 'CHECKPOINT');
is( $preparing_standby->safe_psql('postgres',
		sprintf($built_spills, $prepared_large))
	  . $preparing_standby->safe_psql('postgres',
		sprintf($prepared_chunks, $prepared_large))
	  . $preparing_standby->safe_psql(
		'preparing_other', sprintf($prepared_held, $prepared_small)),
	'000',
	"after a checkpoint, the promoted node's ROLLBACK PREPAREDs leave none of their chunks listed or in the delta in either database"
);
is( $preparing_standby->safe_psql(
		'postgres', 'SELECT count(*), sum(length(v)) FROM toasted_prepared'),
	'20|160000',
	"and the COMMIT PREPARED's values read back whole");
$preparing_standby->stop;

my $restart_rolled = $prepare->($preparing, 'postgres', 'restart_rolled', 20);
my $restart_kept = $prepare->($preparing, 'postgres', 'restart_kept', 20);
$preparing->stop('immediate');
$preparing->start;
is( $preparing->safe_psql('postgres',
		sprintf($prepared_chunks, $restart_rolled))
	  . $preparing->safe_psql('postgres',
		sprintf($prepared_chunks, $restart_kept))
	  . $preparing->safe_psql('preparing_other',
		sprintf($prepared_chunks, $prepared_small))
	  . $preparing->safe_psql('postgres',
		sprintf($prepared_chunks, $prepared_large))
	  . $preparing->safe_psql(
		'postgres', sprintf($prepared_chunks, $prepared_kept)),
	'00000',
	"a primary's crash recovery spills its prepared transactions' chunks out of its delta in either database"
);
$preparing->safe_psql('postgres',
	"ROLLBACK PREPARED 'restart_rolled'; COMMIT PREPARED 'restart_kept'; CHECKPOINT"
);
is( $preparing->safe_psql(
		'postgres', sprintf($built_spills, $restart_rolled)),
	'0',
	"after a checkpoint, its ROLLBACK PREPARED leaves none of its chunks listed"
);
$preparing->safe_psql('preparing_other', "ROLLBACK PREPARED 'small'");
$preparing->safe_psql('postgres',
	"ROLLBACK PREPARED 'large'; COMMIT PREPARED 'kept'");
is( $preparing->safe_psql(
		'postgres', 'SELECT count(*), sum(length(v)) FROM toasted_prepared'),
	'40|320000',
	"and its COMMIT PREPARED's values read back whole");
$preparing->stop;

done_testing();
