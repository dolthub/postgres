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

# A move of a table whose delta holds committed rows past its base and an
# aborted insert's takes past those rows, which a snapshot held from before
# them keeps the move's fold from applying, raises its new storage's tuple id
# counter past every tuple id it carries: the moving transaction's insert, the
# next, and 300 more past the last page take heap's places, (3,41), (3,42), up
# to (5,102), and a standby whose last replayed record is a move and a primary
# that replays a move as its last record after a crash hand out no tuple id a
# row or a carried take holds (bugs row 489).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $settings =
  "fold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off\nallow_in_place_tablespaces = on";
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', $settings);
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
$primary->backup('moved');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'moved', has_streaming => 1);
$standby->append_conf('postgresql.conf', $settings);
$standby->start;
$primary->safe_psql('postgres', "CREATE TABLESPACE moved LOCATION ''");

# Each table gets 100 folded rows; then, under a snapshot held from before
# them, 50 committed rows and an aborted insert of 250 past them.
my @tables = ('t', 'u', 'w');
for my $table (@tables)
{
	$primary->safe_psql('postgres',
		"CREATE TABLE $table (id int4, v text); INSERT INTO $table SELECT g, repeat('x', 30) FROM generate_series(1, 100) g; SELECT doltgres_test_fold('$table')"
	);
}
my $held = $primary->background_psql('postgres');
$held->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
for my $table (@tables)
{
	$primary->safe_psql('postgres',
		"INSERT INTO $table SELECT g, repeat('w', 30) FROM generate_series(101, 150) g"
	);
	$primary->safe_psql('postgres',
		"BEGIN; INSERT INTO $table SELECT g, repeat('y', 30) FROM generate_series(151, 400) g; ROLLBACK"
	);
}

is( $primary->safe_psql(
		'postgres',
		"BEGIN; ALTER TABLE t SET TABLESPACE moved; INSERT INTO t VALUES (0, 'moving') RETURNING ctid; COMMIT"
	),
	'(3,41)',
	"the moving transaction's insert takes heap's place past the aborted insert's");
is( $primary->safe_psql(
		'postgres', "INSERT INTO t VALUES (-1, 'moved') RETURNING ctid"),
	'(3,42)',
	"the next insert after the move takes heap's place");
$primary->safe_psql('postgres',
	"INSERT INTO t SELECT -g, repeat('z', 30) FROM generate_series(10, 309) g");
is( $primary->safe_psql(
		'postgres',
		"SELECT max(ctid)::text || ' ' || (count(*) - count(DISTINCT ctid)) FROM t"),
	'(5,102) 0',
	"300 inserts past the move's last page take heap's fresh pages, no tuple id twice");

# unheld reports whether a node's 300 inserts into a moved table take tuple
# ids no other row holds, each past the aborted insert's last one.
sub unheld
{
	my ($node, $table) = @_;
	$node->safe_psql('postgres',
		"INSERT INTO $table SELECT -g, 'after' FROM generate_series(1, 300) g");
	return $node->safe_psql('postgres',
		"SELECT bool_and(ctid > '(3,40)'::tid) FILTER (WHERE id < 0) AND count(*) = count(DISTINCT ctid) FROM $table"
	);
}

$primary->safe_psql('postgres', 'ALTER TABLE u SET TABLESPACE moved');
$primary->wait_for_replay_catchup($standby);
$standby->promote;
is(unheld($standby, 'u'), 't',
	"a promoted standby whose last replayed record is a move takes no held tuple id"
);
$standby->stop;

$primary->safe_psql('postgres', 'ALTER TABLE w SET TABLESPACE moved');
$held->quit;
$primary->stop('immediate');
$primary->start;
is(unheld($primary, 'w'), 't',
	"a primary that replays a move as its last record after a crash takes no held tuple id"
);

done_testing();
