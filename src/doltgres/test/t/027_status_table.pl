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

# The commit status table (design item 7) in the server's shared memory,
# through the test module's doltgres_test_status_table: an entry one backend
# takes another finds, counts, and removes; enough takes grow the table past
# its first segment, its root's run sized from the server's delta_memory,
# keeping every entry; and a growth paused between its decisions and its copy
# keeps what changed meanwhile.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('status_table');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

# The xids are made in the server, each committed or left running.
my $committed = $node->safe_psql('postgres',
	'BEGIN; SELECT txid_current(); COMMIT;');
is( $node->safe_psql('postgres',
		"SELECT doltgres_test_status_table(0, $committed, 7)"),
	'0',
	'a take of room makes a new entry');
is( $node->safe_psql('postgres',
		"SELECT doltgres_test_status_table(1, $committed, 0), doltgres_test_status_table(4, $committed, 0) = (1::int8 << 62) | 7"
	),
	'0|t',
	'an entry one backend takes another finds');
is( $node->safe_psql('postgres',
		"SELECT doltgres_test_status_table(0, $committed, 9), doltgres_test_status_table(3, $committed, 2), doltgres_test_status_table(3, $committed, -1)"
	),
	'1|2|1',
	'a second take leaves the entry, and its count adds and subtracts');

# A growth: enough committed xids, each counted, take the table past its
# first segment of 4,096 places, and each is found after.
$node->safe_psql('postgres',
	q{CREATE TABLE xids (x int8); DO $$ BEGIN FOR i IN 1..3200 LOOP INSERT INTO xids SELECT txid_current(); COMMIT; END LOOP; END $$});
is( $node->safe_psql('postgres',
		q{SELECT count(*) FILTER (WHERE doltgres_test_status_table(0, x, 0) = 0 AND doltgres_test_status_table(3, x, 1) = 1) FROM xids}
	),
	'3200',
	'a growth past the first segment keeps taking room');
is( $node->safe_psql('postgres',
		q{SELECT count(*) FILTER (WHERE doltgres_test_status_table(1, x, 0) = 1) FROM xids}),
	'3200',
	'every entry is found after the growth');
is( $node->safe_psql('postgres',
		"SELECT doltgres_test_status_table(2, $committed, 0), doltgres_test_status_table(1, $committed, 0)"),
	'1|-1',
	'a removed entry is found no more');

# A growth paused between its decisions and its copy keeps what changed
# meanwhile: an ended entry of zero count, which its first pass marked to
# drop, is counted, and a running entry's transaction, which it didn't
# mark, rolls back. The growth is the one a session's run of takes makes
# at the load factor, paused in that session alone; the run ends once the
# ended entry is counted, before a later growth leaves the aborted one
# behind.
my $ended = $node->safe_psql('postgres', 'BEGIN; SELECT txid_current(); COMMIT;');
$node->safe_psql('postgres', "SELECT doltgres_test_status_table(0, $ended, 0)");
my $doomer = $node->background_psql('postgres');
$doomer->query_safe('BEGIN');
my $doomed = $doomer->query_safe('SELECT txid_current()');
$doomer->query_safe(
	"SELECT doltgres_test_status_table(0, $doomed, 0), doltgres_test_status_table(3, $doomed, 1)");
my $grower = $node->background_psql('postgres');
$grower->query_safe('SELECT injection_points_set_local()');
$grower->query_safe(
	"SELECT injection_points_attach('doltgres-status-grow-decided', 'wait')");
$grower->query_until(
	qr/start/, q{\echo start
DO $$ BEGIN FOR i IN 1..4000 LOOP PERFORM doltgres_test_status_table(0, txid_current(), 0); PERFORM doltgres_test_status_table(3, txid_current(), 1); COMMIT; EXIT WHEN doltgres_test_status_table(1, } . $ended . q{, 0) = 1; END LOOP; END $$;
});
$node->wait_for_event('client backend', 'doltgres-status-grow-decided');
$node->safe_psql('postgres', "SELECT doltgres_test_status_table(3, $ended, 1)");
$doomer->query_safe('ROLLBACK');
$doomer->quit;
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-status-grow-decided')");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-status-grow-decided')");
$grower->quit;
is( $node->safe_psql('postgres',
		"SELECT doltgres_test_status_table(1, $ended, 0), doltgres_test_status_table(1, $doomed, 0)"),
	'1|1',
	'a growth paused between its decisions and its copy keeps what changed meanwhile');

$node->stop;
done_testing();
