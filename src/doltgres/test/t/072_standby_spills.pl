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

done_testing();
