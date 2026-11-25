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

# A streaming standby restarted from a restartpoint whose redo point comes
# before a database's drop or move it replayed (bugs row 374): the records
# past the redo point name the database's storages, whose files and
# repository the replayed drop or move already removed, so the start skips
# them until it replays the drop again, where it once stopped at the
# database's missing repository. Stock Postgres 19 loses a database moved to
# an in-place tablespace the same way in this order, so the moved database's
# own reads aren't this test's; the standby only starts and reads the others.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = 400;
my $half = $rows / 2;
my $wide =
  "(SELECT string_agg(md5(g::text || '.' || k::text), '') FROM generate_series(1, 250) k)";

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'allow_in_place_tablespaces = on');
$primary->start;
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', 'delta_memory = 2MB');
$standby->start;

$primary->safe_psql('postgres',
	"CREATE TABLESPACE moved LOCATION ''; CREATE DATABASE dropped; CREATE DATABASE moving; "
	  . "CREATE TABLE kept (id int4 PRIMARY KEY, v text); INSERT INTO kept SELECT g, md5(g::text) FROM generate_series(1, $rows) g"
);

# Each database's table gets half its rows before the primary's checkpoint and
# half after it; the standby's restartpoint takes that checkpoint's redo point.
foreach my $database ('dropped', 'moving')
{
	$primary->safe_psql($database,
		"CREATE TABLE t (id int4 PRIMARY KEY, v text); INSERT INTO t SELECT g, $wide FROM generate_series(1, $half) g"
	);
}
$primary->safe_psql('postgres', 'CHECKPOINT');
foreach my $database ('dropped', 'moving')
{
	$primary->safe_psql($database,
		"INSERT INTO t SELECT g, $wide FROM generate_series($half + 1, $rows) g; CREATE EXTENSION doltgres_test; SELECT doltgres_test_fold('t')"
	);
}
$primary->wait_for_catchup($standby);
$standby->safe_psql('postgres', 'CHECKPOINT');

# Bugs row 374's cases: the drop and the move after the restartpoint, then
# the standby's crash and start.
$primary->safe_psql('postgres', 'DROP DATABASE dropped');
$primary->safe_psql('postgres', 'ALTER DATABASE moving SET TABLESPACE moved');
$primary->wait_for_catchup($standby);
$standby->stop('immediate');
$standby->start;
$primary->wait_for_catchup($standby);
is( $standby->safe_psql(
		'postgres', 'SELECT count(*), sum(length(v)) FROM kept'),
	"$rows|" . ($rows * 32),
	'the standby restarted past a dropped and a moved database reads its rows (row 374)'
);
is( $standby->safe_psql(
		'postgres',
		'SELECT string_agg(datname, \' \' ORDER BY datname) FROM pg_database'
	),
	'moving postgres template0 template1',
	'the restarted standby lists the moved database and not the dropped one (row 374)'
);

$standby->stop;
$primary->stop;
done_testing();
