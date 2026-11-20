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

# The status door against its contract (design item 7's first
# transaction-state seam), through the test module, with the cases the
# simulation's run against its own table: a transaction is in progress until
# it ends, and then committed or aborted; a subtransaction rolled back is
# aborted while its transaction runs on, and one released is in progress until
# its transaction ends. The in_progress door asked alone agrees, and an xid in a
# later epoch than the next one, which narrows to a running xid's 32 bits, and
# one older than the commit log keeps are in progress to no one. The current
# doors give a savepoint's writer and its transaction's xid, the current
# command, and whether an xid is the current transaction's.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('status');
$node->init;
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins(); CREATE TABLE written (a int4)');

# A transaction is in progress, to itself and to another session, until it
# ends, and then committed or aborted.
my $one = $node->background_psql('postgres');
my $other = $node->background_psql('postgres');
$one->query_safe('BEGIN');
$other->query_safe('BEGIN');
my $first = $one->query_safe('SELECT pg_current_xact_id()::text::int8');
my $second = $other->query_safe('SELECT pg_current_xact_id()::text::int8');
is($one->query_safe("SELECT doltgres_test_status($first)"),
	'0', 'a transaction is in progress to itself');
is($node->safe_psql('postgres', "SELECT doltgres_test_status($second)"),
	'0', "another session's transaction is in progress");
$one->query_safe('COMMIT');
$other->query_safe('ROLLBACK');
is($node->safe_psql('postgres', "SELECT doltgres_test_status($first)"),
	'1', 'a committed transaction is committed');
is($node->safe_psql('postgres', "SELECT doltgres_test_status($second)"),
	'2', 'a rolled back transaction is aborted');

# A subtransaction rolled back is aborted while its transaction runs on, and
# one released is in progress until its transaction ends, with its outcome;
# each subtransaction's xid is the xmin of the row written inside it.
$one->query_safe('BEGIN');
my $top = $one->query_safe('SELECT pg_current_xact_id()::text::int8');
$one->query_safe('SAVEPOINT rolled');
$one->query_safe('INSERT INTO written VALUES (1)');
my $rolled =
  $one->query_safe('SELECT xmin::text::int8 FROM written WHERE a = 1');
$one->query_safe('ROLLBACK TO SAVEPOINT rolled');
$one->query_safe('SAVEPOINT released');
$one->query_safe('INSERT INTO written VALUES (2)');
my $released =
  $one->query_safe('SELECT xmin::text::int8 FROM written WHERE a = 2');
$one->query_safe('RELEASE SAVEPOINT released');
isnt($rolled, $top, 'the rolled back subtransaction has its own xid');
isnt($released, $top, 'the released subtransaction has its own xid');
is($node->safe_psql('postgres', "SELECT doltgres_test_status($rolled)"),
	'2', 'a subtransaction rolled back is aborted while its transaction runs');
is($node->safe_psql('postgres', "SELECT doltgres_test_status($released)"),
	'0', 'a subtransaction released is in progress while its transaction runs'
);
is($node->safe_psql('postgres', "SELECT doltgres_test_status($top)"),
	'0', 'the transaction is in progress');
$one->query_safe('COMMIT');
is($node->safe_psql('postgres', "SELECT doltgres_test_status($released)"),
	'1', 'a subtransaction released is committed with its transaction');
is($node->safe_psql('postgres', "SELECT doltgres_test_status($rolled)"),
	'2', 'a subtransaction rolled back stays aborted');

# The in_progress door: a running transaction is in progress to itself and to
# another session, a committed one isn't, and the same 32 bits in a later epoch
# than the next xid are in progress to no one.
$one->query_safe('BEGIN');
my $running = $one->query_safe('SELECT pg_current_xact_id()::text::int8');
is($one->query_safe("SELECT doltgres_test_in_progress($running)"),
	'1', 'a running transaction is in progress to itself, asked alone');
is($node->safe_psql('postgres', "SELECT doltgres_test_in_progress($running)"),
	'1', "another session's transaction is in progress, asked alone");
my $later = $running + 2**32;
is($node->safe_psql('postgres', "SELECT doltgres_test_in_progress($later)"),
	'0', 'an xid in a later epoch than the next one is in progress to no one');
$one->query_safe('COMMIT');
is($node->safe_psql('postgres', "SELECT doltgres_test_in_progress($running)"),
	'0', 'a committed transaction is in progress to no one, asked alone');
is($node->safe_psql('postgres', "SELECT doltgres_test_in_progress(3)"),
	'0', 'an xid older than the commit log keeps is in progress to no one');

# The current doors: a savepoint's writer is its own xid under its
# transaction's, both are the current transaction's while it runs, a
# subtransaction rolled back no longer is, and another session's never is; a
# command that writes is followed by the next command.
$one->query_safe('BEGIN');
$one->query_safe('SAVEPOINT current');
my $writer = $one->query_safe('SELECT doltgres_test_writer(0)');
my $writing = $one->query_safe('SELECT doltgres_test_writer(1)');
is($one->query_safe('SELECT pg_current_xact_id()::text::int8'),
	$writing, "a savepoint's writer is its own xid under its transaction's");
ok($writer > $writing, "the savepoint's xid follows its transaction's");
$other->query_safe('BEGIN');
my $others = $other->query_safe('SELECT pg_current_xact_id()::text::int8');
is($one->query_safe("SELECT doltgres_test_is_current($writer)"),
	'1', "the savepoint's xid is the current transaction's");
is($one->query_safe("SELECT doltgres_test_is_current($others)"),
	'0', "another session's xid is not the current transaction's");
$one->query_safe('ROLLBACK TO SAVEPOINT current');
is($one->query_safe("SELECT doltgres_test_is_current($writer)"),
	'0', "a subtransaction rolled back is no longer the current transaction's");
is($one->query_safe("SELECT doltgres_test_is_current($writing)"),
	'1', 'its transaction still is');
$one->query_safe('ROLLBACK');
$other->query_safe('ROLLBACK');
$one->query_safe('BEGIN');
is($one->query_safe('SELECT doltgres_test_command(0)'),
	'0', 'a transaction starts at its first command');
$one->query_safe('INSERT INTO written VALUES (3)');
is($one->query_safe('SELECT doltgres_test_command(0)'),
	'1', 'a command that writes is followed by the next command');
is($one->query_safe('SELECT doltgres_test_command(1)'),
	'1', 'a command marked used keeps its id');
is($one->query_safe('SELECT doltgres_test_command(0)'),
	'2', 'and is followed by the next command');
$one->query_safe('ROLLBACK');
$one->quit;
$other->quit;

# The xids no transaction has are answered with no read of the commit log:
# the bootstrap and frozen xids are committed and the invalid one aborted,
# each in progress to no one.
is( $node->safe_psql(
		'postgres',
		'SELECT doltgres_test_status(0), doltgres_test_status(1), doltgres_test_status(2)'
	),
	'2|1|1',
	'the bootstrap and frozen xids are committed and the invalid one aborted'
);
is( $node->safe_psql(
		'postgres',
		'SELECT doltgres_test_in_progress(0), doltgres_test_in_progress(1), doltgres_test_in_progress(2)'
	),
	'0|0|0',
	'and each is in progress to no one');

$node->stop;
done_testing();
