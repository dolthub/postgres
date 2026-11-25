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
# database's move a second time, over the repository its first replay already
# moved, and folds again a table its first replay's fold never wrote, since the
# move's conflict ended that fold worker; every row reads after the second
# replay (bugs row 443).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"allow_in_place_tablespaces = on\ncheckpoint_timeout = 1d");
$primary->start;
$primary->safe_psql('postgres',
	'CREATE EXTENSION injection_points; CREATE EXTENSION pg_walinspect');
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf',
	"checkpoint_timeout = 1d\nmax_wal_size = 10GB\ndelta_memory = 1MB");
$standby->start;
$standby->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-fold-relation', 'wait')");
$primary->safe_psql('postgres', "CREATE TABLESPACE moved LOCATION ''");
$primary->safe_psql('postgres', 'CREATE DATABASE replayed');
$primary->safe_psql('replayed',
	'CREATE EXTENSION doltgres_test; CREATE TABLE spilled (k int4, v text); INSERT INTO spilled SELECT g, md5(g::text) || md5((-g)::text) FROM generate_series(1, 20000) g'
);
$primary->safe_psql('replayed', "SELECT doltgres_test_fold('spilled')");
$standby->wait_for_event('fold worker', 'doltgres-fold-relation');
my $before =
  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$primary->safe_psql('postgres',
	'ALTER DATABASE replayed SET TABLESPACE moved');
$primary->wait_for_replay_catchup($standby);

# The move's redo flushes through its record before its finish changes the old
# directory, so a restarted standby takes connections only past it.
my $moved = $primary->safe_psql('postgres',
	"SELECT max(end_lsn) FROM pg_get_wal_records_info('$before', pg_current_wal_insert_lsn()) WHERE record_type = 'RepositoryMoved'"
);
is( $standby->safe_psql(
		'postgres',
		"SELECT min_recovery_end_lsn >= '$moved' FROM pg_control_recovery()"),
	't',
	"the standby's minimum recovery point passes the move's record (row 443)"
);
$standby->stop('immediate');

# A standby whose startup process dies after pg_ctl's start returns never
# catches up, so the wait fails the test.
$standby->start;
$primary->wait_for_replay_catchup($standby);
my ($ret, $stdout, $stderr) =
  $standby->psql('replayed', "SELECT count(*) || ' ' || sum(k) FROM spilled");
is( "$stdout$stderr",
	'20000 200010000',
	"a table whose fold the move's conflict ended reads after a crash replays the move again (row 443)"
);
$standby->stop;
$primary->stop;

done_testing();
