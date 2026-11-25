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

# A standby that replays a database's move to another tablespace reads every
# row of a table the primary folded before the move (bugs row 449), though
# its own fold of the table may run between the move's commit and its
# finish; and a standby that crashes inside the copy its replay of the
# finish makes, its copy's directory made and empty, reads every row after
# its restart.

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
	"allow_in_place_tablespaces = on\ncheckpoint_timeout = 1d\nfold_threshold = 1GB\nfold_timeout = 3600s"
);
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

# moved makes a database with a folded table and moves it to a tablespace of
# its own.
sub moved
{
	my ($name) = @_;
	$primary->safe_psql('postgres', "CREATE TABLESPACE $name LOCATION ''");
	$primary->safe_psql('postgres', "CREATE DATABASE $name");
	$primary->safe_psql($name,
		'CREATE EXTENSION doltgres_test; CREATE TABLE folded (k int4); INSERT INTO folded SELECT generate_series(1, 100)'
	);
	$primary->safe_psql($name, "SELECT doltgres_test_fold('folded')");
	$primary->safe_psql('postgres',
		"ALTER DATABASE $name SET TABLESPACE $name");
}

# read_rows returns the standby's count and sum of a database's folded table,
# with any error after.
sub read_rows
{
	my ($name) = @_;
	my ($ret, $stdout, $stderr) =
	  $standby->psql($name, "SELECT count(*) || ' ' || sum(k) FROM folded");
	return "$stdout$stderr";
}

moved('replayed');
$primary->wait_for_replay_catchup($standby);
is(read_rows('replayed'), '100 5050',
	'a standby reads a folded table after it replays the move (row 449)');

$standby->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-packs-copy-made', 'wait')");
moved('crashed');
$standby->wait_for_event('startup', 'doltgres-packs-copy-made');
$standby->stop('immediate');
$standby->start;
$primary->wait_for_replay_catchup($standby);
is( read_rows('crashed'),
	'100 5050',
	"a standby reads a folded table after a crash inside its replayed finish's copy (row 449)"
);
$standby->stop;
$primary->stop;

done_testing();
