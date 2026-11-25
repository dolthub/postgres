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

# A crash inside ALTER DATABASE ... SET TABLESPACE's finish loses nothing
# (bugs row 442). A table whose first rows a fold put in its base and whose
# later rows only the delta holds, and a table only the delta holds, read
# every row after the start, for a crash right after the copy's rename and
# one right after the manifest's rekey, each of whose finishes the start
# completes from the old directory's mark, and for a crash after the finish
# took the mark away and before the move's checkpoint, whose rekeyed manifest
# the start loads as it stands.

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
	"fold_threshold = 1GB\nfold_timeout = 3600s\nallow_in_place_tablespaces = on\ncheckpoint_timeout = 1d"
);
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

# No checkpoint comes before the move: a checkpoint asks the fold workers to
# fold every table with a base, and movedb's lock keeps a worker out of the
# database until its finish, so the folded table keeps both its base and its
# delta through the move.
for my $form (
	[
		'renamed', 'doltgres-store-move-renamed',
		"right after the copy's rename"
	],
	[
		'rekeyed', 'doltgres-store-move-rekeyed',
		"right after the manifest's rekey"
	],
	[
		'finished',
		'doltgres-store-move-finished',
		"after the finish and before the move's checkpoint"
	])
{
	my ($name, $point, $where) = @$form;
	$node->safe_psql('postgres', "CREATE TABLESPACE moved_$name LOCATION ''");
	$node->safe_psql('postgres', "CREATE DATABASE moved_$name");
	$node->safe_psql("moved_$name",
		'CREATE EXTENSION doltgres_test; CREATE TABLE folded (k int4); INSERT INTO folded SELECT generate_series(1, 100)'
	);
	$node->safe_psql("moved_$name", "SELECT doltgres_test_fold('folded')");
	$node->safe_psql("moved_$name",
		'INSERT INTO folded SELECT generate_series(101, 200); CREATE TABLE delta (k int4); INSERT INTO delta SELECT generate_series(1, 100)'
	);
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('$point', 'wait')");
	my $mover = $node->background_psql('postgres', on_error_stop => 0);
	$mover->query_until(qr/start/,
		"\\echo start\nALTER DATABASE moved_$name SET TABLESPACE moved_$name;\n"
	);
	$node->wait_for_event('client backend', $point);
	$node->stop('immediate');
	eval { $mover->quit };
	$node->start;
	my ($ret, $stdout, $stderr) = $node->psql("moved_$name",
		"SELECT count(*) || ' ' || sum(k) FROM folded");
	is( "$stdout$stderr",
		'200 20100',
		"every folded and unfolded row reads after a crash $where (row 442)");
	($ret, $stdout, $stderr) = $node->psql("moved_$name",
		"SELECT count(*) || ' ' || sum(k) FROM delta");
	is( "$stdout$stderr",
		'100 5050',
		"every row only the delta held reads after a crash $where (row 442)");
}
$node->stop;

done_testing();
