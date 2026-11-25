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

# A streaming standby reads the indexes its primary builds (bugs row 366):
# CREATE INDEX over short and over out-of-line text, CREATE INDEX
# CONCURRENTLY, REINDEX, VACUUM FULL, and CLUSTER each build an index whose
# base reaches the standby through its Chunk records, so the standby's
# reads through it, and after its promotion, give the primary's rows, where
# the standby once installed a base whose packs it never had and read nothing.
# The standby's restartpoint at a checkpoint of the primary's that a CLUSTER
# ran inside reads them too after a crash (bugs row 374), where its start
# once remade the clustered table's old storage from the records past the
# checkpoint's redo point alone and stopped at the fold's mismatch; the
# primary's own crash past the same checkpoint, while the old storage's
# file still stands, keeps reading them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = 400;
my $wide =
  "(SELECT string_agg(md5(g::text || '.' || k::text), '') FROM generate_series(1, 250) k)";

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"max_wal_size = 4GB\ncheckpoint_timeout = 1d");
$primary->start;
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', 'delta_memory = 2MB');
$standby->start;

# filled makes a table of text, short or wide, its index made before or
# after its rows.
sub filled
{
	my ($table, $value, $index_first) = @_;
	my $index = "CREATE INDEX ${table}_v ON $table (v);";
	my $insert =
	  "INSERT INTO $table SELECT g, $value FROM generate_series(1, $rows) g;";
	$primary->safe_psql('postgres',
		"CREATE TABLE $table (id int4 PRIMARY KEY, v text); "
		  . ($index_first ? "$index $insert" : "$insert $index"));
}

filled('narrow', 'md5(g::text)', 0);
filled('created', $wide, 0);
filled('reindexed', $wide, 1);
$primary->safe_psql('postgres', 'REINDEX INDEX reindexed_v');
filled('vacuumed', $wide, 1);
$primary->safe_psql('postgres', 'VACUUM FULL vacuumed');
$primary->safe_psql('postgres',
	"CREATE TABLE concurrent (id int4 PRIMARY KEY, v text); INSERT INTO concurrent SELECT g, $wide FROM generate_series(1, $rows) g"
);
$primary->safe_psql('postgres',
	'CREATE INDEX CONCURRENTLY concurrent_v ON concurrent (v)');

# The clustered table's rows go in two halves around a checkpoint of the
# primary's held after it sets its redo point and before it saves, and its
# CLUSTER runs before the checkpoint finishes, as a checkpoint the WAL's
# growth starts does; with no injection points it is filled at once.
my $held = $ENV{enable_injection_points} eq 'yes'
  && $primary->check_extension('injection_points');
my $half = $rows / 2;
if ($held)
{
	$primary->safe_psql('postgres',
		"CREATE TABLE clustered (id int4 PRIMARY KEY, v text); CREATE INDEX clustered_v ON clustered (v); "
		  . "INSERT INTO clustered SELECT g, $wide FROM generate_series(1, $half) g; "
		  . "CREATE EXTENSION injection_points; "
		  . "SELECT injection_points_attach('create-checkpoint-initial', 'wait'); "
		  . "SELECT injection_points_attach('create-checkpoint-run', 'wait')"
	);
	my $checkpoint = $primary->background_psql('postgres');
	$checkpoint->query_until(qr/started/, "\\echo started\nCHECKPOINT;\n");
	$primary->wait_for_event('checkpointer', 'create-checkpoint-initial');
	$primary->safe_psql('postgres',
		"SELECT injection_points_wakeup('create-checkpoint-initial')");
	$primary->wait_for_event('checkpointer', 'create-checkpoint-run');
	$primary->safe_psql('postgres',
		"INSERT INTO clustered SELECT g, $wide FROM generate_series($half + 1, $rows) g; "
		  . 'CLUSTER clustered USING clustered_pkey');
	my $log_offset = -s $primary->logfile;
	$primary->safe_psql('postgres',
			"SELECT injection_points_detach('create-checkpoint-initial'); "
		  . "SELECT injection_points_detach('create-checkpoint-run'); "
		  . "SELECT injection_points_wakeup('create-checkpoint-run')");
	$primary->wait_for_log(qr/checkpoint complete/, $log_offset);
	$checkpoint->quit;
}
else
{
	filled('clustered', $wide, 1);
	$primary->safe_psql('postgres', 'CLUSTER clustered USING clustered_pkey');
}
$primary->wait_for_catchup($standby);

my @tables = qw(narrow created reindexed vacuumed clustered concurrent);
my %bitmap = (
	settings => 'SET enable_seqscan = off; SET enable_indexscan = off;',
	plan => 'Bitmap Index Scan on');
my %index = (
	settings => 'SET enable_seqscan = off; SET enable_bitmapscan = off;',
	plan => 'Index (Only )?Scan using');
my %table = (
	settings => 'SET enable_indexscan = off; SET enable_bitmapscan = off;',
	plan => 'Seq Scan on');

# read_through gives a node's count and length of a table's rows by a scan of
# a kind, after asserting the plan scans that way: through the table's index,
# or the table alone.
sub read_through
{
	my ($node, $table, $scan, $when) = @_;
	my $query = "SELECT count(*), sum(length(v)) FROM $table WHERE v >= ''";
	my $read = $scan == \%table ? $table : "${table}_v";
	like(
		$node->safe_psql(
			'postgres', "$scan->{settings} EXPLAIN (COSTS OFF) $query"),
		qr/$scan->{plan} $read\b/,
		"$table\'s read on the standby $when scans $read");
	my ($ret, $stdout, $stderr) =
	  $node->psql('postgres', "$scan->{settings} $query");
	return "$stdout$stderr";
}

# compared asserts that the primary reads each table's rows through its index,
# by a bitmap scan and an index scan, and from the table alone, and that the
# standby reads them as the primary does.
sub compared
{
	my ($when) = @_;
	foreach my $table (@tables)
	{
		foreach my $scan (\%bitmap, \%index, \%table)
		{
			my $wanted = $primary->safe_psql('postgres',
				"$scan->{settings} SELECT count(*), sum(length(v)) FROM $table WHERE v >= ''"
			);
			is( $wanted,
				"$rows|" . ($rows * ($table eq 'narrow' ? 32 : 8000)),
				"the primary reads $table\'s rows by $scan->{plan} $when");
			is( read_through($standby, $table, $scan, $when),
				$wanted,
				"the standby reads $table\'s rows by $scan->{plan} $when as its primary does (row 366)"
			);
		}
	}
}

# Bugs row 366's cases: right after the standby catches up, then after its
# promotion. Bugs row 374's: after the standby's restartpoint at the held
# checkpoint and a crash, then the primary's crash past it.
compared('after catch-up');
SKIP:
{
	skip 'injection points not supported by this build', 1 + 9 * @tables
	  unless $held;
	$standby->safe_psql('postgres', 'CHECKPOINT');
	$standby->stop('immediate');
	$standby->start;
	$primary->wait_for_catchup($standby);
	compared('after a restartpoint and a crash');
	$primary->stop('immediate');
	$primary->start;
	is( $primary->safe_psql(
			'postgres', "SELECT count(*), sum(length(v)) FROM clustered"),
		"$rows|" . ($rows * 8000),
		'the primary reads clustered\'s rows after a crash past its CLUSTER (row 374)'
	);
	$primary->wait_for_catchup($standby);
}
$standby->promote;
compared('after its promotion');

$standby->stop;
$primary->stop;
done_testing();
