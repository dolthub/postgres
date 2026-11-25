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

# A standby that crashes with no restartpoint since its base backup replays a
# database's move a second time, after its first replay folded a table made
# in pg_default after the move into packs of pg_default's directory of the
# database, the one the move left; the table reads after the second replay
# (bugs row 454).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"allow_in_place_tablespaces = on\ncheckpoint_timeout = 1d");
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION pg_walinspect');
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf',
	"checkpoint_timeout = 1d\nmax_wal_size = 10GB");
$standby->start;
$primary->safe_psql('postgres', "CREATE TABLESPACE moved LOCATION ''");
$primary->safe_psql('postgres', 'CREATE DATABASE rereplayed');
my $before =
  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$primary->safe_psql('postgres',
	'ALTER DATABASE rereplayed SET TABLESPACE moved');
my $moved = $primary->safe_psql('postgres',
	"SELECT max(end_lsn) FROM pg_get_wal_records_info('$before', pg_current_wal_insert_lsn()) WHERE record_type = 'RepositoryMoved'"
);
$primary->safe_psql('rereplayed',
	'CREATE EXTENSION doltgres_test; CREATE TABLE kept_default (k int4) TABLESPACE pg_default; INSERT INTO kept_default SELECT generate_series(1, 100)'
);
$primary->safe_psql('rereplayed',
	"SELECT doltgres_test_fold('kept_default')");
$primary->wait_for_replay_catchup($standby);
my $database = $standby->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'rereplayed'");
my $dir = $standby->data_dir . "/pg_versioned/$database";
my $folded = 0;

foreach (1 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
{
	$folded = () = glob("$dir/*.pack");
	last if $folded > 0;
	usleep(100_000);
}
ok( $folded > 0,
	"the standby's fold wrote the pg_default table's packs in pg_default's directory of the database"
);

# The restart replays the move again only from a redo point before it, which no
# restartpoint has passed.
is( $standby->safe_psql(
		'postgres', "SELECT redo_lsn < '$moved' FROM pg_control_checkpoint()"
	),
	't',
	"the standby's redo point precedes the move's record before the crash (row 454)"
);
$standby->stop('immediate');

# A standby whose startup process dies after pg_ctl's start returns never
# catches up, so the wait fails the test.
$standby->start;
$primary->wait_for_replay_catchup($standby);
my ($ret, $stdout, $stderr) = $standby->psql('rereplayed',
	"SELECT count(*) || ' ' || sum(k) FROM kept_default");
is( "$stdout$stderr",
	'100 5050',
	"a pg_default table made after a move reads after a crash replays the move again (row 454)"
);
$standby->stop;
$primary->stop;

done_testing();
