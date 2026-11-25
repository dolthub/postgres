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

# A primary's crashes inside a database's move (bugs row 443): right after the
# move's finish wrote the move's log into its copy, before its mark, the open
# that finds the move committed finishes it and the moved repository's log
# holds the move once; and after the finish, before the move's checkpoint,
# recovery replays no record of the database's storages before the move, since
# movedb checkpoints before its copy and refuses while the database is in use.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('crashed');
$node->init;
$node->append_conf('postgresql.conf',
	"allow_in_place_tablespaces = on\ncheckpoint_timeout = 1d\ndelta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s"
);
$node->start;
$node->safe_psql('postgres',
	"CREATE EXTENSION injection_points; CREATE EXTENSION pg_walinspect; CREATE TABLESPACE moved LOCATION ''"
);

# moved_crash moves a database into the moved tablespace, crashing the server
# at a pause of the move's finish and starting it again; check runs at the
# pause.
sub moved_crash
{
	my ($database, $pause, $check) = @_;
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('$pause', 'wait')");
	my $mover = $node->background_psql('postgres', on_error_stop => 0);
	$mover->query_until(qr/start/,
		"\\echo start\nALTER DATABASE $database SET TABLESPACE moved;\n");
	$node->wait_for_event('client backend', $pause);
	$check->() if $check;
	$node->stop('immediate');
	eval { $mover->quit };
	$node->start;
}

# rows returns a moved database's table's count and sum, with any error after.
sub rows
{
	my ($database) = @_;
	my ($ret, $stdout, $stderr) =
	  $node->psql($database, "SELECT count(*) || ' ' || sum(k) FROM spilled");
	return "$stdout$stderr";
}

foreach my $database ('logged', 'finished')
{
	$node->safe_psql('postgres', "CREATE DATABASE $database");
	$node->safe_psql($database,
		'CREATE TABLE spilled (k int4, v text); INSERT INTO spilled SELECT g, md5(g::text) || md5((-g)::text) FROM generate_series(1, 20000) g'
	);
}

moved_crash('logged', 'doltgres-store-move-logged');
is( rows('logged'),
	'20000 200010000',
	"every row reads after a crash right after the move's log is written (row 443)"
);
my $logged = $node->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'logged'");
my @logs =
  glob($node->data_dir . "/pg_tblspc/*/*/pg_versioned/$logged/moves");
is(scalar(@logs) == 1 ? -s $logs[0] : -1,
	21, "the moved repository's log holds the move once (row 443)");

my $finished = $node->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'finished'");
moved_crash(
	'finished',
	'doltgres-store-move-finished',
	sub {
		is( $node->safe_psql(
				'postgres',
				"SELECT count(*) FROM pg_get_wal_records_info((SELECT redo_lsn FROM pg_control_checkpoint()), pg_current_wal_insert_lsn()) WHERE resource_manager = 'Versioned' AND description LIKE '%/$finished/%'"
			),
			'0',
			"no record of the database's storages lies between the redo point and the move (row 443)"
		);
	});
is( rows('finished'),
	'20000 200010000',
	"every row reads after a crash after the move's finish and before its checkpoint (row 443)"
);
$node->stop;

done_testing();
