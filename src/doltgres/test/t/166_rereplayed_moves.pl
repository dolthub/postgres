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

# A standby that crashes with no restartpoint since its base backup replays two
# moves of a database a second time, over the repository its first replay
# already moved twice: from pg_default to another tablespace and back, and from
# pg_default to one tablespace and then another with a table made in pg_default
# after both, which the standby folded; every row reads after the second replay
# (bugs rows 454 and 466).

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
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf',
	"checkpoint_timeout = 1d\nmax_wal_size = 10GB");
$standby->start;
$primary->safe_psql('postgres', "CREATE TABLESPACE $_ LOCATION ''")
  foreach ('moved_a', 'moved_b');

foreach my $database ('back', 'onward')
{
	$primary->safe_psql('postgres', "CREATE DATABASE $database");
	$primary->safe_psql($database,
		'CREATE EXTENSION doltgres_test; CREATE TABLE kept (k int4); INSERT INTO kept SELECT generate_series(1, 100)'
	);
	$primary->safe_psql($database, "SELECT doltgres_test_fold('kept')");
}
$primary->safe_psql('postgres',
	"ALTER DATABASE $_->[0] SET TABLESPACE $_->[1]")
  foreach (
	[ 'back', 'moved_a' ],
	[ 'back', 'pg_default' ],
	[ 'onward', 'moved_a' ],
	[ 'onward', 'moved_b' ]);
$primary->safe_psql('onward',
	'CREATE TABLE kept_default (k int4) TABLESPACE pg_default; INSERT INTO kept_default SELECT generate_series(1, 100)'
);
$primary->safe_psql('onward', "SELECT doltgres_test_fold('kept_default')");
$primary->wait_for_replay_catchup($standby);
my $onward = $standby->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'onward'");
my $folded = 0;

foreach (1 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
{
	$folded = () = glob($standby->data_dir . "/pg_versioned/$onward/*.pack");
	last if $folded > 0;
	usleep(100_000);
}
ok( $folded > 0,
	"the standby's fold wrote the pg_default table's packs in pg_default's directory of the database"
);
$standby->stop('immediate');

# A standby whose startup process dies after pg_ctl's start returns never
# catches up, so the wait fails the test.
$standby->start;
$primary->wait_for_replay_catchup($standby);

# rows returns a table's count and sum on the standby, with any error after.
sub rows
{
	my ($database, $table) = @_;
	my ($ret, $stdout, $stderr) =
	  $standby->psql($database,
		"SELECT count(*) || ' ' || sum(k) FROM $table");
	return "$stdout$stderr";
}
is( rows('back', 'kept'),
	'100 5050',
	"a database moved away and back reads after a crash replays both moves again (rows 454 and 466)"
);
is( rows('onward', 'kept'),
	'100 5050',
	"a database moved twice reads after a crash replays both moves again (rows 454 and 466)"
);
is(rows('onward', 'kept_default'),
	'100 5050',
	"and its pg_default table made after both moves (rows 454 and 466)");
$standby->stop;
$primary->stop;

done_testing();
