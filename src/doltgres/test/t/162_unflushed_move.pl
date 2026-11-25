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

# A standby follows a database's move whose primary crashes right after the
# move's copy rename (bugs row 448): the primary's start finishes the move
# from the old directory's mark, and rows written into the moved database
# after the start read on the primary and on the standby, before and after
# its promotion, since the move flushed its record before the rename.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# The move's record follows movedb's commit, which ForceSyncCommit flushed.
# Nothing else flushes it before the crash: no checkpoint or autovacuum runs,
# the WAL writer waits before its background flush, and the background writer
# logs no standby snapshot, whose flush request would wake the WAL writer. So
# only the move's own flush sends the record to the standby.
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"allow_in_place_tablespaces = on\nautovacuum = off\ncheckpoint_timeout = 1d\nfold_threshold = 1GB\nfold_timeout = 3600s"
);
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;
$primary->safe_psql('postgres', "CREATE TABLESPACE moved LOCATION ''");
$primary->safe_psql('postgres', 'CREATE DATABASE unflushed');
$primary->safe_psql('unflushed',
	'CREATE TABLE rows_kept (k int4); INSERT INTO rows_kept SELECT generate_series(1, 100)'
);
$primary->wait_for_replay_catchup($standby);
$primary->safe_psql('postgres',
	"SELECT injection_points_attach('walwriter-before-background-flush', 'wait'); SELECT injection_points_attach('skip-log-running-xacts', 'error')"
);
$primary->wait_for_event('walwriter', 'walwriter-before-background-flush');
$primary->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-store-move-renamed', 'wait')");
my $mover = $primary->background_psql('postgres', on_error_stop => 0);
$mover->query_until(qr/start/,
	"\\echo start\nALTER DATABASE unflushed SET TABLESPACE moved;\n");
$primary->wait_for_event('client backend', 'doltgres-store-move-renamed');
$primary->stop('immediate');
eval { $mover->quit };
$primary->start;
$primary->safe_psql('unflushed',
	'INSERT INTO rows_kept SELECT generate_series(101, 200)');
is( $primary->safe_psql(
		'unflushed', "SELECT count(*) || ' ' || sum(k) FROM rows_kept"),
	'200 20100',
	'the primary reads every row after its start finished the move');
$primary->wait_for_replay_catchup($standby);

# read_rows returns the standby's count and sum of the moved database's table,
# with any error after.
sub read_rows
{
	my ($ret, $stdout, $stderr) = $standby->psql('unflushed',
		"SELECT count(*) || ' ' || sum(k) FROM rows_kept");
	return "$stdout$stderr";
}

is( read_rows(),
	'200 20100',
	"a standby reads every row of a database whose primary crashed right after the move's copy rename (row 448)"
);
$standby->promote;
is(read_rows(), '200 20100', 'and after its promotion (row 448)');
$standby->stop;
$primary->stop;

done_testing();
