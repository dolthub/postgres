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

# A database moved to another tablespace keeps its spilled versions (bugs row
# 438): a table's committed versions spill out of a 1MB delta with no fold to
# take them, ALTER DATABASE ... SET TABLESPACE moves the database, and every
# row reads back after the move, and after a checkpoint and a restart. A
# table moved by ALTER TABLE ... SET TABLESPACE keeps its spilled versions
# too. A crash before the move's rekey, with spills a checkpoint listed and
# spills only the replay remakes, of a table the checkpoint had and of one
# made after it, reads every row after the start; and a standby that
# replays the move reads every row its own replay spilled.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = 20000;
my $settings =
  "delta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s\nallow_in_place_tablespaces = on";

# filled makes a table of a database and spills its rows, numbered from a
# first one, in one transaction.
sub filled
{
	my ($node, $database, $table, $first) = @_;
	my $last = $first + $rows - 1;
	$node->safe_psql($database, "CREATE TABLE IF NOT EXISTS $table (k int4, v text)");
	$node->safe_psql($database,
		"INSERT INTO $table SELECT g, md5(g::text) || md5((-g)::text) FROM generate_series($first, $last) g",
		timeout => 600);
}

# counted returns a table's count of rows, their sum, and the count whose value
# is right, with any error after.
sub counted
{
	my ($node, $database, $table) = @_;
	my ($ret, $stdout, $stderr) = $node->psql($database,
		"SELECT count(*) || ' ' || sum(k) || ' ' || count(*) FILTER (WHERE v = md5(k::text) || md5((-k)::text)) FROM $table"
	);
	return "$stdout$stderr";
}

# all returns what counted returns for the rows numbered 1 to a count.
sub all
{
	my ($count) = @_;
	return "$count " . ($count * ($count + 1) / 2) . " $count";
}

my $node = PostgreSQL::Test::Cluster->new('moved');
$node->init;
$node->append_conf('postgresql.conf', $settings);
$node->start;
$node->safe_psql('postgres', "CREATE TABLESPACE moved LOCATION ''");
$node->safe_psql('postgres', 'CREATE DATABASE spilled');
my $database = $node->safe_psql('postgres', "SELECT oid FROM pg_database WHERE datname = 'spilled'");
filled($node, 'spilled', 'spilled_rows', 1);
my $pgdata = $node->data_dir;
my @spills = glob("$pgdata/pg_versioned/$database/delta/spill/rows-*");
ok(@spills > 0, "the table's committed versions spilled");
is(counted($node, 'spilled', 'spilled_rows'), all($rows), 'every row reads before the move');
$node->safe_psql('postgres', 'ALTER DATABASE spilled SET TABLESPACE moved');
is(counted($node, 'spilled', 'spilled_rows'),
	all($rows), 'every row reads after the database moves to another tablespace (row 438)');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->restart;
is(counted($node, 'spilled', 'spilled_rows'),
	all($rows), 'every row reads after the move, a checkpoint, and a restart (row 438)');

# A table's own move makes a new storage and carries the old one's spilled
# versions into it.
$node->safe_psql('postgres', "CREATE TABLESPACE aside LOCATION ''");
filled($node, 'postgres', 'aside_rows', 1);
$node->safe_psql('postgres', 'ALTER TABLE aside_rows SET TABLESPACE aside');
is(counted($node, 'postgres', 'aside_rows'),
	all($rows), 'every spilled row reads after ALTER TABLE SET TABLESPACE (row 438)');
$node->restart;
is(counted($node, 'postgres', 'aside_rows'),
	all($rows), 'every spilled row reads after ALTER TABLE SET TABLESPACE and a restart (row 438)');
$node->stop;

SKIP:
{
	skip 'Injection points not supported by this build', 2
	  unless $ENV{enable_injection_points} eq 'yes';

	# A crash before the rekey: the table spilled before a checkpoint, then
	# twice past it, the second time a table the checkpoint never had.
	my $crashed = PostgreSQL::Test::Cluster->new('crashed');
	$crashed->init;
	$crashed->append_conf('postgresql.conf', "$settings\ncheckpoint_timeout = 1d");
	$crashed->start;
	$crashed->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	$crashed->safe_psql('postgres', "CREATE TABLESPACE crashed LOCATION ''");
	$crashed->safe_psql('postgres', 'CREATE DATABASE crashed');
	filled($crashed, 'crashed', 'listed_rows', 1);
	$crashed->safe_psql('postgres', 'CHECKPOINT');
	filled($crashed, 'crashed', 'listed_rows', $rows + 1);
	filled($crashed, 'crashed', 'late_rows', 1);
	$crashed->safe_psql('postgres', "SELECT injection_points_attach('doltgres-store-move-finishing', 'wait')");
	my $mover = $crashed->background_psql('postgres', on_error_stop => 0);
	$mover->query_until(qr/start/, "\\echo start\nALTER DATABASE crashed SET TABLESPACE crashed;\n");
	$crashed->wait_for_event('client backend', 'doltgres-store-move-finishing');
	$crashed->stop('immediate');
	eval { $mover->quit };
	$crashed->start;
	is(counted($crashed, 'crashed', 'listed_rows'),
		all(2 * $rows), 'every row of a table the checkpoint listed reads after a crash before the rekey (row 438)');
	is(counted($crashed, 'crashed', 'late_rows'),
		all($rows), 'every row of a table made past the checkpoint reads after a crash before the rekey (row 438)');
	$crashed->stop;
}

# A standby replays the move of a database whose rows its own replay spilled.
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
$primary->safe_psql('postgres', 'CREATE DATABASE spilled');
filled($primary, 'spilled', 'spilled_rows', 1);
$primary->wait_for_replay_catchup($standby);
$database = $standby->safe_psql('postgres', "SELECT oid FROM pg_database WHERE datname = 'spilled'");
@spills = glob($standby->data_dir . "/pg_versioned/$database/delta/spill/rows-*");
ok(@spills > 0, "the standby's replay spilled the table's versions");
$primary->safe_psql('postgres', 'ALTER DATABASE spilled SET TABLESPACE moved');
$primary->wait_for_replay_catchup($standby);
is(counted($standby, 'spilled', 'spilled_rows'),
	all($rows), 'a standby reads every row its replay spilled after it replays the move (row 438)');
$standby->stop;
$primary->stop;

done_testing();
