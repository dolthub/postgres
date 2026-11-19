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

# The commit status table across a crash and on a hot standby (bugs row 90): a
# start loads every entry of its checkpoint's status run though the table grows
# during the load, and keeps a prepared transaction's entry of zero count
# through a growth during crash recovery, before the end of recovery puts the
# prepared transaction back in the proc array, so a fold after it reads each
# loaded writer's commit; a hot standby's growth still leaves an ended entry
# behind.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('status');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 2');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

# committed commits a number of transactions, each inserting one row into a
# fork table through the fork's routine, so each holds a status entry until a
# fold; 4,700 pass the 4,608 entries at which the table's first segment grows.
sub committed
{
	my ($table, $from, $count) = @_;
	$node->safe_psql(
		'postgres', qq{
DO \$\$
BEGIN
	FOR n IN $from .. $from + $count - 1 LOOP
		PERFORM doltgres_test_write('$table', true, format('INSERT INTO $table VALUES (%s, ''v'')', n));
		COMMIT;
	END LOOP;
END
\$\$;
});
}

# A checkpoint whose status run holds 4,700 committed transactions' entries,
# then a crash: the start's load grows the table before any loaded version is
# counted, keeps every entry, and a fold reads each writer's commit.
$node->safe_psql('postgres',
	'CREATE TABLE loaded (k int4 PRIMARY KEY, v text)');
committed('loaded', 1, 4700);
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('loaded')"),
	't', "a start keeps every entry of a status run that grows the table");
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM doltgres_test_scan('loaded', false, -1)"),
	'4700',
	'the folded rows read back');

# A prepared transaction that only locked a row holds an entry of zero count. A
# checkpoint names it, 4,700 transactions after it commit, and a crash: redo's
# entries grow the table before the end of recovery puts the prepared
# transaction back in the proc array, the growth keeps its entry, and COMMIT
# PREPARED and a fold read its commit.
$node->safe_psql('postgres',
	'CREATE TABLE locked (k int4 PRIMARY KEY, v text)');
committed('locked', 1, 1);
$node->safe_psql(
	'postgres', q{
BEGIN;
SELECT doltgres_test_write('locked', true, 'SELECT * FROM locked WHERE k = 1 FOR UPDATE');
PREPARE TRANSACTION 'locker';
});
$node->safe_psql('postgres', 'CHECKPOINT');
committed('locked', 2, 4700);
$node->stop('immediate');
$node->start;
$node->safe_psql('postgres', "COMMIT PREPARED 'locker'");
is( $node->safe_psql('postgres', "SELECT doltgres_test_fold('locked')"),
	't',
	"a prepared transaction's entry outlives the status table's growth during crash recovery"
);

# A hot standby's proc array answers for the primary's running and prepared
# transactions through its known-assigned xids, so a growth during its replay
# still leaves behind an entry of zero count whose transaction ended: the
# entry a standby session takes for a transaction the primary committed is
# gone once the replay of 4,700 later transactions grows the table.
$node->backup('standing');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'standing', has_streaming => 1);
$standby->start;
my $ended = $node->safe_psql('postgres', 'SELECT pg_current_xact_id()');
$node->safe_psql('postgres',
	'CREATE TABLE replayed (k int4 PRIMARY KEY, v text)');
$node->wait_for_replay_catchup($standby);
is( $standby->safe_psql(
		'postgres', "SELECT doltgres_test_status_table(0, $ended, 0)"),
	'0',
	"a standby session takes an entry for a transaction the primary committed"
);
committed('replayed', 1, 4700);
$node->wait_for_replay_catchup($standby);
is( $standby->safe_psql(
		'postgres', "SELECT doltgres_test_status_table(1, $ended, 0)"),
	'-1',
	"a growth during a hot standby's replay leaves an ended entry of zero count behind"
);
$standby->stop;

$node->stop;
done_testing();
