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

# A built base's out-of-line values outlive a checkpoint and a crash (bugs
# row 364): CREATE INDEX, VACUUM FULL, CLUSTER, and a primary key's rebuild
# each build an index over text too wide to stay in its entries, and the
# last three copy the table's rows, whose values the delta may still hold,
# into a new base, under a delta small enough that the values spill out of
# it, where the build once left them for the checkpoint to drop.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = 400;

# built makes a server and a table of wide text, its index made before or
# after its rows, runs a build over it, checkpoints, crashes, and restarts,
# and says whether the index reads every row's value.
sub built
{
	my ($name, $index_first, $build) = @_;
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init;
	$node->append_conf('postgresql.conf', 'delta_memory = 2MB');
	$node->start;
	my $index = 'CREATE INDEX wide_v ON wide (v);';
	my $insert =
	  "INSERT INTO wide SELECT g, (SELECT string_agg(md5(g::text || '.' || k::text), '') FROM generate_series(1, 250) k) FROM generate_series(1, $rows) g;";
	$node->safe_psql('postgres',
		'CREATE TABLE wide (id int4 PRIMARY KEY, v text); '
		  . ($index_first ? "$index $insert" : "$insert $index"));
	$node->safe_psql('postgres', $build) if defined $build;
	$node->safe_psql('postgres', 'CHECKPOINT');
	$node->stop('immediate');
	$node->start;
	my $settings = 'SET enable_seqscan = off; SET enable_indexscan = off;';
	my $query = "SELECT count(*), sum(length(v)) FROM wide WHERE v >= ''";
	like(
		$node->safe_psql('postgres', "$settings EXPLAIN (COSTS OFF) $query"),
		qr/Bitmap Index Scan on wide_v/,
		"$name\'s count reads its index through a bitmap scan");
	my ($ret, $stdout, $stderr) = $node->psql('postgres', "$settings $query");
	is( $stdout,
		"$rows|" . ($rows * 8000),
		"$name\'s index reads every row's value after a checkpoint and a crash (row 364): $stderr"
	);
	my $heap = 'SET enable_indexscan = off; SET enable_bitmapscan = off;';
	like(
		$node->safe_psql('postgres', "$heap EXPLAIN (COSTS OFF) $query"),
		qr/Seq Scan on wide/,
		"$name\'s table count reads the table alone");
	($ret, $stdout, $stderr) = $node->psql('postgres', "$heap $query");
	is( $stdout,
		"$rows|" . ($rows * 8000),
		"$name\'s table reads every row's value after a checkpoint and a crash (row 364): $stderr"
	);
	$node->stop;
}

# Bugs row 364's cases, one server each.
built('created', 0, undef);
built('vacuumed', 1, 'VACUUM FULL wide');
built('clustered', 1, 'CLUSTER wide USING wide_pkey');
built('rekeyed', 1,
	'ALTER TABLE wide DROP CONSTRAINT wide_pkey; ALTER TABLE wide ADD PRIMARY KEY (id)'
);

# A folded base's out-of-line values outlive the checkpoint that takes their
# transaction's chunk spills out (bugs row 387): one INSERT spills its values,
# a fold writes the base naming them, and under wal_level minimal the 127
# databases made after bring the checkpoint to take the spills out, which once
# left the base naming chunks nothing held.
my $folded = PostgreSQL::Test::Cluster->new('folded');
$folded->init;
$folded->append_conf('postgresql.conf', "wal_level = minimal\nmax_wal_senders = 0");
$folded->start;
my $wide = "(SELECT string_agg(md5(g::text || '.' || k::text), '') FROM generate_series(1, 70) k)";
$folded->safe_psql('postgres', 'CREATE TABLE outside (id int4 PRIMARY KEY, v text)');
$folded->safe_psql('postgres', "INSERT INTO outside SELECT g, $wide FROM generate_series(1, 4000) g");
$folded->safe_psql('postgres', "CREATE DATABASE other_$_") for 1 .. 127;
my ($folded_ret, $folded_out, $folded_err) = $folded->psql('postgres',
	"SELECT count(*) FROM outside WHERE v = " . ($wide =~ s/g::text/id::text/r));
is($folded_out, '4000', "every folded row's value reads back after its spills are taken out (row 387): $folded_err");
$folded->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
$folded->safe_psql('postgres', 'CREATE TABLE placed (id int4 PRIMARY KEY, v text)');
$folded->safe_psql('postgres', "INSERT INTO placed SELECT g, $wide FROM generate_series(1, 4000) g");
$folded->safe_psql('postgres', "SELECT doltgres_test_fold('placed')");
$folded->safe_psql('postgres', 'CHECKPOINT');
is($folded->safe_psql('postgres', "SELECT doltgres_test_unplaced('placed')"),
	'0', 'a folded base names no chunk only the delta or its chunk spills hold (row 387)');
$folded->stop;

# A streaming standby's fold places the values it replayed too (bugs row 387):
# the standby replays one INSERT of out-of-line values, folds until its base
# is its primary's, with no mismatch, and after a restartpoint and a crash
# reads every value through a base that names no chunk only its delta or its
# chunk spills hold.
my $streamed = PostgreSQL::Test::Cluster->new('streamed');
$streamed->init(allows_streaming => 1);
$streamed->start;
$streamed->safe_psql('postgres',
	'CREATE EXTENSION doltgres_test; CREATE TABLE replayed (id int4 PRIMARY KEY, v text)');
$streamed->backup('streamed_backup');
my $replica = PostgreSQL::Test::Cluster->new('replica');
$replica->init_from_backup($streamed, 'streamed_backup', has_streaming => 1);
$replica->start;
my $replica_offset = -s $replica->logfile;
$streamed->safe_psql('postgres', "INSERT INTO replayed SELECT g, $wide FROM generate_series(1, 4000) g");
$streamed->safe_psql('postgres', "SELECT doltgres_test_fold('replayed')");
$streamed->safe_psql('postgres', 'SELECT txid_current()');
$streamed->wait_for_catchup($replica);
my $streamed_sizes = $streamed->safe_psql('postgres', "SELECT doltgres_test_base_sizes('replayed')");
ok( $replica->poll_query_until(
		'postgres',
		"SELECT doltgres_test_base_sizes('replayed') = '$streamed_sizes' FROM (SELECT doltgres_test_fold('replayed')) f"
	),
	"the standby folds the replayed values until its base is its primary's");
$replica->safe_psql('postgres', 'CHECKPOINT');
$replica->stop('immediate');
$replica->start;
$streamed->wait_for_catchup($replica);
my ($replica_ret, $replica_out, $replica_err) = $replica->psql('postgres',
	"SELECT count(*) FROM replayed WHERE v = " . ($wide =~ s/g::text/id::text/r));
is($replica_out, '4000',
	"the standby reads every replayed value after a restartpoint and a crash (row 387): $replica_err");
is($replica->safe_psql('postgres', "SELECT doltgres_test_unplaced('replayed')"),
	'0', "the standby's folded base names no chunk only its delta or its chunk spills hold (row 387)");
ok(!$replica->log_contains(qr/does not match its primary's/, $replica_offset),
	"the standby's fold matches its primary's (row 387)");
$replica->stop;
$streamed->stop;

done_testing();
