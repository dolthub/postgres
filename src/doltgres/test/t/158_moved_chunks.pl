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

# A database moved to another tablespace keeps its chunk spills where its
# repository stands (bugs row 441): a transaction's out-of-line values spill
# their chunks out of a 1MB delta, before the database's move and after it,
# and every value reads back after the move, and after a restart; and a
# standby whose replay spilled the chunks replays the move and reads every
# value, and again after a restartpoint and its restart.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = 400;
my $settings = "delta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s\nallow_in_place_tablespaces = on";
my $node = PostgreSQL::Test::Cluster->new('chunks');
$node->init;
$node->append_conf('postgresql.conf', $settings);
$node->start;

# filled makes a database's table of out-of-line values and fills it in one
# transaction, on a node or the test's own.
sub filled
{
	my ($database, $on) = @_;
	$on //= $node;
	$on->safe_psql($database,
		'CREATE TABLE large (k int4, v text); ALTER TABLE large ALTER COLUMN v SET STORAGE EXTERNAL');
	$on->safe_psql($database,
		"INSERT INTO large SELECT g, repeat(md5(g::text), 600) FROM generate_series(1, $rows) g",
		timeout => 600);
}

# spilled returns the count of a database's chunk spill files under a
# directory of repositories, on a node or the test's own.
sub spilled
{
	my ($versioned, $database, $on) = @_;
	$on //= $node;
	my $oid = $on->safe_psql('postgres', "SELECT oid FROM pg_database WHERE datname = '$database'");
	my @files = glob("$versioned/$oid/delta/spill/chunks-*");
	return scalar @files;
}

# counted returns a database's count of values, their sum of lengths, and the
# count of right ones, with any error after, on a node or the test's own.
sub counted
{
	my ($database, $on) = @_;
	$on //= $node;
	my ($ret, $stdout, $stderr) = $on->psql($database,
		"SELECT count(*) || ' ' || sum(length(v)) || ' ' || count(*) FILTER (WHERE v = repeat(md5(k::text), 600)) FROM large"
	);
	return "$stdout$stderr";
}

my $all = "$rows " . ($rows * 19200) . " $rows";
my $pgdata = $node->data_dir;
$node->safe_psql('postgres', "CREATE TABLESPACE before_move LOCATION ''");
$node->safe_psql('postgres', "CREATE TABLESPACE after_move LOCATION ''");

# Chunks spilled before the move.
$node->safe_psql('postgres', 'CREATE DATABASE spilled_before');
filled('spilled_before');
cmp_ok(spilled("$pgdata/pg_versioned", 'spilled_before'), '>', 0, "the transaction's chunks spilled");
is(counted('spilled_before'), $all, 'every value reads before the move');
$node->safe_psql('postgres', 'ALTER DATABASE spilled_before SET TABLESPACE before_move');
is(counted('spilled_before'), $all, 'every value whose chunks spilled before the move reads after it (row 441)');
$node->restart;
is(counted('spilled_before'), $all, 'and after a restart (row 441)');

# Chunks spilled after the move.
$node->safe_psql('postgres', 'CREATE DATABASE spilled_after');
$node->safe_psql('spilled_after', 'SELECT 1');
$node->safe_psql('postgres', 'ALTER DATABASE spilled_after SET TABLESPACE after_move');
filled('spilled_after');
my $space = $node->safe_psql('postgres', "SELECT oid FROM pg_tablespace WHERE spcname = 'after_move'");
my ($versioned) = glob("$pgdata/pg_tblspc/$space/PG_*/pg_versioned");
cmp_ok(spilled($versioned, 'spilled_after'), '>', 0,
	"the transaction's chunks spilled where the moved repository stands (row 441)");
is(counted('spilled_after'), $all, 'every value whose chunks spilled after the move reads (row 441)');
$node->restart;
is(counted('spilled_after'), $all, 'and after a restart (row 441)');
$node->stop;

# A standby whose replay spilled the chunks replays the move. Its restart
# follows a restartpoint past the move, which a restart before replays
# again (bugs row 443).
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', "allow_in_place_tablespaces = on\ncheckpoint_timeout = 1d");
$primary->start;
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', $settings);
$standby->start;
$primary->safe_psql('postgres', "CREATE TABLESPACE moved LOCATION ''");
$primary->safe_psql('postgres', 'CREATE DATABASE replayed');
filled('replayed', $primary);
$primary->wait_for_replay_catchup($standby);
cmp_ok(spilled($standby->data_dir . '/pg_versioned', 'replayed', $standby),
	'>', 0, "the standby's replay spilled the transaction's chunks");
$primary->safe_psql('postgres', 'ALTER DATABASE replayed SET TABLESPACE moved');
$primary->wait_for_replay_catchup($standby);
is(counted('replayed', $standby), $all, 'a standby reads every value its replay spilled after it replays the move (row 441)');
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->wait_for_replay_catchup($standby);
$standby->safe_psql('postgres', 'CHECKPOINT');
$standby->restart;
is(counted('replayed', $standby), $all, 'and after a restartpoint and its restart (row 441)');
$standby->stop;
$primary->stop;

done_testing();
